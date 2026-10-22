#include "session.h"

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

using namespace winrt::Windows::Graphics::Capture;
using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;
using winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;

namespace {
constexpr int kPoolFrames = 3;    // WGC buffers: one we hold, one in flight, one being written
constexpr int kEncodeFrames = 24;  // NV12 surfaces the encoder can hold at once
constexpr int kMaxWidth = 4096;    // widest H.264 frame NVENC, AMF and QSV encode
const auto kFormat = DirectXPixelFormat::B8G8R8A8UIntNormalized;

com_ptr<ID3D11Texture2D> TextureOf(const Direct3D11CaptureFrame& f) {
  auto access = f.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
  com_ptr<ID3D11Texture2D> t;
  winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(t.put())));
  return t;
}

GraphicsCaptureItem ItemFor(HWND hwnd) {
  auto interop = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
  GraphicsCaptureItem item{nullptr};
  winrt::check_hresult(
      interop->CreateForWindow(hwnd, winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(item)));
  return item;
}

// Lets us turn off the yellow capture border. Must not block an STA thread, so it runs on its own.
void RequestBorderless() {
  static std::once_flag once;
  std::call_once(once, [] {
    std::thread([] {
      try {
        winrt::init_apartment();
        GraphicsCaptureAccess::RequestAccessAsync(GraphicsCaptureAccessKind::Borderless).get();
      } catch (...) {
      }
    }).join();
  });
}

// Fit (sw x sh) inside (w x h), centred, keeping aspect.
RECT Fit(UINT sw, UINT sh, int w, int h) {
  int64_t dw = w, dh = int64_t(sh) * w / sw;
  if (dh > h) dh = h, dw = int64_t(sw) * h / sh;
  LONG x = LONG(w - dw) / 2, y = LONG(h - dh) / 2;
  return {x, y, x + LONG(dw), y + LONG(dh)};
}
}  // namespace

VideoCapture::VideoCapture(HWND hwnd, const Config& cfg, PacketRing& ring, int64_t t0,
                           std::function<void()> on_closed)
    : ring_(ring), t0_(t0), fps_(cfg.fps), on_closed_(std::move(on_closed)) {
  try {
    RequestBorderless();
    CreateDevice(cfg.gpu_priority);
    auto item = ItemFor(hwnd);
    // Output size is fixed for the session; a resized or recreated window is letterboxed into it.
    auto sz = item.Size();
    if (sz.Width <= 0 || sz.Height <= 0) throw std::runtime_error("window has no size");
    h_ = std::min(sz.Height, cfg.max_height) & ~1;
    w_ = int(int64_t(sz.Width) * h_ / sz.Height) & ~1;
    if (w_ > kMaxWidth) {  // super-ultrawide: H.264 encoders stop at 4096 wide
      w_ = kMaxWidth;
      h_ = int(int64_t(sz.Height) * w_ / sz.Width) & ~1;
    }
    OpenEncoder(cfg);
    track_ = ring_.AddTrack("Video", enc_, true);
    StartCapture(item);
  } catch (...) {
    StopCapture();
    Free();
    throw;
  }
  thread_ = std::thread(&VideoCapture::Loop, this);
}

VideoCapture::~VideoCapture() {
  stop_ = true;
  if (thread_.joinable()) thread_.join();
  StopCapture();
  Free();
}

void VideoCapture::Free() {
  av_frame_free(&last_);
  avcodec_free_context(&enc_);
  av_buffer_unref(&hw_frames_);
  av_buffer_unref(&hw_dev_);
}

void VideoCapture::CreateDevice(int gpu_priority) {
  auto d = CreateVideoDevice(gpu_priority);  // WGC, the encoder and our thread all use it
  dev_ = d.dev;
  ctx_ = d.ctx;
  vdev_ = dev_.as<ID3D11VideoDevice>();
  vctx_ = ctx_.as<ID3D11VideoContext1>();

  com_ptr<::IInspectable> insp;
  winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dev_.as<IDXGIDevice>().get(), insp.put()));
  rt_dev_ = insp.as<IDirect3DDevice>();
}

void VideoCapture::OpenEncoder(const Config& cfg) {
  hw_dev_ = FfmpegDevice(dev_.get());
  hw_frames_ = EncoderSurfaces(hw_dev_, w_, h_, kEncodeFrames);
  std::string name;
  enc_ = OpenH264(hw_frames_, w_, h_, {fps_, 1}, cfg.bitrate_mbps * int64_t(1000000), fps_, name);  // 1 s keyframes
  reuse_ = name == "h264_nvenc";  // the only encoder tested with one surface queued twice
  Log("encoder {} {}x{} {} fps {} Mbit/s", name, w_, h_, fps_, cfg.bitrate_mbps);
}

void VideoCapture::StartCapture(GraphicsCaptureItem item) {
  pool_size_ = item.Size();
  pool_ = Direct3D11CaptureFramePool::CreateFreeThreaded(rt_dev_, kFormat, kPoolFrames, pool_size_);
  // WGC calls these on its own threads, possibly while StopCapture runs: the gate makes StopCapture wait
  // for one in progress, and keeps a closed pool's late frames out.
  gate_ = std::make_shared<Gate>();
  gate_->owner = this;
  pool_.FrameArrived([gate = gate_](const Direct3D11CaptureFramePool& pool, auto&&) {
    std::lock_guard g(gate->m);
    if (gate->owner) gate->owner->OnFrame(pool);
  });
  closed_token_ = item.Closed([gate = gate_](auto&&, auto&&) {
    std::lock_guard g(gate->m);
    if (gate->owner) gate->owner->on_closed_();
  });
  session_ = pool_.CreateCaptureSession(item);
  try {
    session_.IsBorderRequired(false);
  } catch (...) {
  }
  try {
    session_.MinUpdateInterval(winrt::Windows::Foundation::TimeSpan(10'000'000 / fps_));
  } catch (...) {  // Windows 11 24H2+
  }
  session_.StartCapture();
  item_ = item;
}

void VideoCapture::OnFrame(const Direct3D11CaptureFramePool& pool) {
  try {
    auto f = pool.TryGetNextFrame();
    if (!f) return;
    auto cs = f.ContentSize();
    if (cs.Width <= 0 || cs.Height <= 0) return;  // minimized or mid-change: keep the last real frame
    std::lock_guard l(frame_m_);
    latest_ = f;
    if (cs != pool_size_) {  // window resized: new buffers, the processor letterboxes
      pool_size_ = cs;
      pool.Recreate(rt_dev_, kFormat, kPoolFrames, cs);
    }
  } catch (...) {
  }
}

void VideoCapture::StopCapture() {
  if (gate_) {
    std::lock_guard g(gate_->m);
    gate_->owner = nullptr;
  }
  try {
    if (session_) session_.Close();
    if (pool_) pool_.Close();
    if (item_) item_.Closed(closed_token_);
  } catch (...) {
  }
  session_ = nullptr;
  pool_ = nullptr;
  item_ = nullptr;
  std::lock_guard l(frame_m_);
  latest_ = nullptr;
}

void VideoCapture::SetWindow(HWND hwnd) {
  StopCapture();
  try {
    StartCapture(ItemFor(hwnd));
    Log("following new game window");
  } catch (const winrt::hresult_error& e) {
    Log("can't capture new game window: {}", winrt::to_string(e.message()));
  }
}

// Steady frame rate: encode the newest captured frame on every tick, repeating it if nothing new
// arrived, and skip ticks rather than stall if we fall behind.
void VideoCapture::Loop() {
  winrt::init_apartment();
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  const int64_t f = QpcFreq();
  // Stats for the log: ticks encoded, ticks skipped because we fell behind, time spent encoding.
  int64_t encoded = 0, skipped = 0, busy = 0, since = QpcNow();
  int64_t fails = 0, fail_window = 0;
  for (int64_t n = 0; !stop_;) {
    const int64_t due = t0_ + n * f / fps_;
    const int64_t wait = due - QpcNow();
    if (wait > 0) {
      LARGE_INTEGER li;
      li.QuadPart = -QpcTo(wait, 10'000'000);
      SetWaitableTimerEx(timer, &li, 0, nullptr, nullptr, nullptr, 0);
      WaitForSingleObject(timer, INFINITE);
    } else if (-wait > 2 * f / fps_) {
      const int64_t next = (QpcNow() - t0_) * fps_ / f + 1;
      skipped += next - n;
      n = next;
      continue;
    }
    const int64_t start = QpcNow();
    // A frame that fails now and then (a window being swapped) is skipped; a second's worth of failures within
    // two seconds restarts the session, which loses the buffer.
    if (!Encode(n)) {
      if (start - fail_window > 2 * f) fail_window = start, fails = 0;
      if (++fails >= fps_) {
        failed_ = true;
        break;
      }
    }
    busy += QpcNow() - start;
    encoded++;
    n++;
    if (const int64_t now = QpcNow(); now - since >= 30 * f) {
      Log("video: {:.1f} fps encoded, {:.0f}% repeated frames, {} skipped, {:.2f} ms per frame",
          encoded * double(f) / double(now - since), encoded ? 100.0 * repeats_ / double(encoded) : 0.0, skipped,
          encoded ? QpcTo(busy, 1'000'000) / 1000.0 / double(encoded) : 0.0);
      encoded = skipped = busy = repeats_ = 0;
      since = now;
    }
  }
  CloseHandle(timer);
}

bool VideoCapture::Encode(int64_t pts) {
  Direct3D11CaptureFrame frame{nullptr};
  std::shared_ptr<InputOverlay> overlay;
  {
    std::lock_guard l(frame_m_);
    frame = latest_;  // our reference keeps the texture out of WGC's hands until we're done
    overlay = overlay_;
  }
  if (!frame) return true;  // nothing captured yet

  AVFrame* out = av_frame_alloc();
  try {
    const int64_t frame_time = frame.SystemRelativeTime().count();  // throws once the pool is closed
    const bool repeat = frame_time == last_frame_time_;  // the game hasn't presented since last tick
    if (repeat) repeats_++;
    last_frame_time_ = frame_time;
    // Same picture: hand the encoder the surface we already converted, no GPU work for this tick.
    if (repeat && !overlay && last_ && last_->buf[0]) {
      CheckAv(av_frame_ref(out, last_), "reuse frame");
      out->pts = pts;
      CheckAv(avcodec_send_frame(enc_, out), "encode");
      av_frame_free(&out);
      ring_.Drain(track_, enc_);
      return true;
    }
    auto tex = TextureOf(frame);
    D3D11_TEXTURE2D_DESC d;
    tex->GetDesc(&d);
    auto cs = frame.ContentSize();
    const UINT cw = std::min<UINT>(cs.Width, d.Width), ch = std::min<UINT>(cs.Height, d.Height);
    if (!cw || !ch) {  // nothing to show; Fit would divide by zero
      av_frame_free(&out);
      return true;
    }
    if (!vp_ || d.Width != vp_w_ || d.Height != vp_h_) CreateProcessor(d.Width, d.Height);

    CheckAv(av_hwframe_get_buffer(hw_frames_, out, 0), "get encoder surface");
    auto* out_view = OutputViewFor(out_views_, vdev_.get(), venum_.get(), reinterpret_cast<ID3D11Texture2D*>(out->data[0]),
                                   UINT(intptr_t(out->data[1])));

    const RECT src{0, 0, LONG(cw), LONG(ch)}, dst = Fit(cw, ch, w_, h_);
    vctx_->VideoProcessorSetStreamSourceRect(vp_.get(), 0, TRUE, &src);
    vctx_->VideoProcessorSetStreamDestRect(vp_.get(), 0, TRUE, &dst);
    // Draw the overlay on a copy (WGC's own texture is shared with the capture), or copy if the
    // processor can't read WGC textures at all.
    auto* in = overlay || use_copy_ ? nullptr : InputView(tex.get());
    if (!in) {
      if (!overlay && !use_copy_) Log("capture textures aren't readable by the video processor; copying each frame");
      use_copy_ = use_copy_ || !overlay;
      CopyFrame(tex.get());
      if (overlay) DrawOverlay(*overlay);
      in = InputView(copy_.get());
      if (!in) throw std::runtime_error("video processor can't read frames");
    }
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = in;
    winrt::check_hresult(vctx_->VideoProcessorBlt(vp_.get(), out_view, 0, 1, &stream));

    out->pts = pts;
    out->duration = 1;
    if (reuse_) {
      if (!last_) last_ = av_frame_alloc();
      av_frame_unref(last_);
      if (!overlay) CheckAv(av_frame_ref(last_, out), "keep frame");
    }
    CheckAv(avcodec_send_frame(enc_, out), "encode");
    av_frame_free(&out);
    ring_.Drain(track_, enc_);
    return true;
  } catch (const winrt::hresult_error& e) {
    Log("video failed: {}", winrt::to_string(e.message()));
  } catch (const std::exception& e) {
    Log("video failed: {}", e.what());
  }
  av_frame_free(&out);
  return false;
}

void VideoCapture::CreateProcessor(UINT in_w, UINT in_h) {
  in_views_.clear();
  out_views_.clear();
  copy_ = nullptr;
  d2d_target_ = nullptr;
  vp_ = nullptr;
  venum_ = nullptr;
  D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
  cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
  cd.InputFrameRate = {UINT(fps_), 1};
  cd.InputWidth = in_w;
  cd.InputHeight = in_h;
  cd.OutputFrameRate = {UINT(fps_), 1};
  cd.OutputWidth = w_;
  cd.OutputHeight = h_;
  cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
  winrt::check_hresult(vdev_->CreateVideoProcessorEnumerator(&cd, venum_.put()));
  winrt::check_hresult(vdev_->CreateVideoProcessor(venum_.get(), 0, vp_.put()));
  vctx_->VideoProcessorSetStreamColorSpace1(vp_.get(), 0, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
  vctx_->VideoProcessorSetOutputColorSpace1(vp_.get(), DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
  vctx_->VideoProcessorSetStreamFrameFormat(vp_.get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
  vctx_->VideoProcessorSetStreamAutoProcessingMode(vp_.get(), 0, FALSE);
  D3D11_VIDEO_COLOR black{};
  black.YCbCr = {0.0625f, 0.5f, 0.5f, 1.0f};
  vctx_->VideoProcessorSetOutputBackgroundColor(vp_.get(), TRUE, &black);
  vp_w_ = in_w;
  vp_h_ = in_h;
  Log("video processor {}x{} -> {}x{}", in_w, in_h, w_, h_);
}

ID3D11VideoProcessorInputView* VideoCapture::InputView(ID3D11Texture2D* tex) {
  for (auto& [t, v] : in_views_)
    if (t.get() == tex) return v.get();
  D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC vd{};
  vd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
  com_ptr<ID3D11VideoProcessorInputView> v;
  if (FAILED(vdev_->CreateVideoProcessorInputView(tex, venum_.get(), &vd, v.put()))) return nullptr;
  if (in_views_.size() > 2 * kPoolFrames) in_views_.clear();  // pool was recreated
  com_ptr<ID3D11Texture2D> t;
  t.copy_from(tex);
  in_views_.emplace_back(t, v);
  return v.get();
}

void VideoCapture::CopyFrame(ID3D11Texture2D* tex) {
  if (!copy_) {
    D3D11_TEXTURE2D_DESC d;
    tex->GetDesc(&d);
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    d.MiscFlags = 0;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.CPUAccessFlags = 0;
    winrt::check_hresult(dev_->CreateTexture2D(&d, nullptr, copy_.put()));
    d2d_target_ = nullptr;
  }
  ctx_->CopyResource(copy_.get(), tex);
}

void VideoCapture::EnsureD2D() {
  if (d2d_) return;
  com_ptr<ID2D1Factory1> factory;
  D2D1_FACTORY_OPTIONS fo{};
  winrt::check_hresult(
      D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory1), &fo, factory.put_void()));
  com_ptr<ID2D1Device> d2d_dev;
  winrt::check_hresult(factory->CreateDevice(dev_.as<IDXGIDevice>().get(), d2d_dev.put()));
  winrt::check_hresult(d2d_dev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, d2d_.put()));
}

// Renders the mark once with Direct2D: a dark badge with a red record dot, "Clipped with Clip" and the project link.
void VideoCapture::DrawOverlay(InputOverlay& overlay) {
  EnsureD2D();
  if (!d2d_target_) {
    const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                               D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    winrt::check_hresult(d2d_->CreateBitmapFromDxgiSurface(copy_.as<IDXGISurface>().get(), &props, d2d_target_.put()));
  }
  d2d_->SetTarget(d2d_target_.get());
  d2d_->BeginDraw();
  HRESULT hr;
  try {
    overlay.Draw(d2d_.get(), d2d_target_->GetSize());
    hr = d2d_->EndDraw();
  } catch (const winrt::hresult_error& e) {  // the overlay's own resources: same as a failed draw
    d2d_->EndDraw();
    hr = e.code();
  }
  d2d_->SetTarget(nullptr);
  if (FAILED(hr)) {  // device trouble: rebuild next frame rather than stop recording
    Log("overlay draw failed (0x{:08x})", uint32_t(hr));
    d2d_target_ = nullptr;
    d2d_ = nullptr;
  }
}

void VideoCapture::SetOverlay(std::shared_ptr<InputOverlay> overlay) {
  std::lock_guard l(frame_m_);
  overlay_ = std::move(overlay);
}
