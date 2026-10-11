#include "session.h"

#include <dxgi1_6.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}

using namespace winrt::Windows::Graphics::Capture;
using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;
using winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;

namespace {
constexpr int kPoolFrames = 3;    // WGC buffers: one we hold, one in flight, one being written
constexpr int kEncodeFrames = 24;  // NV12 surfaces the encoder can hold at once
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
    CreateDevice();
    auto item = ItemFor(hwnd);
    // Output size is fixed for the session; a resized or recreated window is letterboxed into it.
    auto sz = item.Size();
    if (sz.Width <= 0 || sz.Height <= 0) throw std::runtime_error("window has no size");
    h_ = std::min(sz.Height, cfg.max_height) & ~1;
    w_ = int(int64_t(sz.Width) * h_ / sz.Height) & ~1;
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
  avcodec_free_context(&enc_);
  av_buffer_unref(&hw_frames_);
  av_buffer_unref(&hw_dev_);
}

void VideoCapture::CreateDevice() {
  // The high-performance GPU is the one games render on, and the one with the good encoder.
  com_ptr<IDXGIFactory6> factory;
  winrt::check_hresult(CreateDXGIFactory2(0, IID_PPV_ARGS(factory.put())));
  com_ptr<IDXGIAdapter1> adapter;
  winrt::check_hresult(
      factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(adapter.put())));
  DXGI_ADAPTER_DESC1 desc;
  adapter->GetDesc1(&desc);
  Log("GPU: {}", Utf8(desc.Description));

  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  winrt::check_hresult(D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                         D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                         levels, 2, D3D11_SDK_VERSION, dev_.put(), nullptr, ctx_.put()));
  // WGC, the encoder and our thread all use this device.
  dev_.as<ID3D11Multithread>()->SetMultithreadProtected(TRUE);
  vdev_ = dev_.as<ID3D11VideoDevice>();
  vctx_ = ctx_.as<ID3D11VideoContext1>();

  com_ptr<::IInspectable> insp;
  winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dev_.as<IDXGIDevice>().get(), insp.put()));
  rt_dev_ = insp.as<IDirect3DDevice>();
}

void VideoCapture::OpenEncoder(const Config& cfg) {
  hw_dev_ = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
  auto* dc = static_cast<AVD3D11VADeviceContext*>(reinterpret_cast<AVHWDeviceContext*>(hw_dev_->data)->hwctx);
  dc->device = dev_.get();
  dev_->AddRef();  // FFmpeg releases it
  CheckAv(av_hwdevice_ctx_init(hw_dev_), "init D3D11 for FFmpeg");

  hw_frames_ = av_hwframe_ctx_alloc(hw_dev_);
  auto* fc = reinterpret_cast<AVHWFramesContext*>(hw_frames_->data);
  fc->format = AV_PIX_FMT_D3D11;
  fc->sw_format = AV_PIX_FMT_NV12;
  fc->width = w_;
  fc->height = h_;
  fc->initial_pool_size = kEncodeFrames;
  static_cast<AVD3D11VAFramesContext*>(fc->hwctx)->BindFlags = D3D11_BIND_RENDER_TARGET;  // video processor output
  CheckAv(av_hwframe_ctx_init(hw_frames_), "create encoder surfaces");

  const int64_t bitrate = cfg.bitrate_mbps * int64_t(1000000);
  for (std::string_view name : {"h264_nvenc", "h264_amf", "h264_qsv"}) {
    const AVCodec* codec = avcodec_find_encoder_by_name(name.data());
    if (!codec) continue;
    AVCodecContext* e = avcodec_alloc_context3(codec);
    e->width = w_;
    e->height = h_;
    e->time_base = {1, fps_};
    e->framerate = {fps_, 1};
    e->pix_fmt = AV_PIX_FMT_D3D11;
    e->sw_pix_fmt = AV_PIX_FMT_NV12;
    e->hw_frames_ctx = av_buffer_ref(hw_frames_);
    e->bit_rate = e->rc_max_rate = bitrate;
    e->rc_buffer_size = int(bitrate);  // 1 s VBV
    e->gop_size = fps_;                // 1 s keyframes
    e->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    e->color_range = AVCOL_RANGE_MPEG;
    e->colorspace = AVCOL_SPC_BT709;
    e->color_primaries = AVCOL_PRI_BT709;
    e->color_trc = AVCOL_TRC_BT709;
    AVDictionary* o = nullptr;
    if (name == "h264_nvenc") {
      av_dict_set(&o, "preset", "p4", 0);
      av_dict_set(&o, "tune", "hq", 0);
      av_dict_set(&o, "rc", "cbr", 0);
    } else if (name == "h264_amf") {
      av_dict_set(&o, "rc", "cbr", 0);
      av_dict_set(&o, "quality", "balanced", 0);
    }
    int r = avcodec_open2(e, codec, &o);
    av_dict_free(&o);
    if (r >= 0) {
      enc_ = e;
      Log("encoder {} {}x{} {} fps {} Mbit/s", name, w_, h_, fps_, cfg.bitrate_mbps);
      return;
    }
    Log("encoder {} unavailable", name);
    avcodec_free_context(&e);
  }
  throw std::runtime_error("no hardware H.264 encoder found on this GPU");
}

void VideoCapture::StartCapture(GraphicsCaptureItem item) {
  pool_size_ = item.Size();
  pool_ = Direct3D11CaptureFramePool::CreateFreeThreaded(rt_dev_, kFormat, kPoolFrames, pool_size_);
  pool_.FrameArrived([this](const Direct3D11CaptureFramePool& pool, auto&&) {
    try {
      auto f = pool.TryGetNextFrame();
      if (!f) return;
      auto cs = f.ContentSize();
      std::lock_guard l(frame_m_);
      latest_ = f;
      if (cs != pool_size_) {  // window resized: new buffers, the processor letterboxes
        pool_size_ = cs;
        pool.Recreate(rt_dev_, kFormat, kPoolFrames, cs);
      }
    } catch (...) {
    }
  });
  closed_token_ = item.Closed([this](auto&&, auto&&) { on_closed_(); });
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

void VideoCapture::StopCapture() {
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
  for (int64_t n = 0; !stop_;) {
    const int64_t due = t0_ + n * f / fps_;
    const int64_t wait = due - QpcNow();
    if (wait > 0) {
      LARGE_INTEGER li;
      li.QuadPart = -QpcTo(wait, 10'000'000);
      SetWaitableTimerEx(timer, &li, 0, nullptr, nullptr, nullptr, 0);
      WaitForSingleObject(timer, INFINITE);
    } else if (-wait > 2 * f / fps_) {
      n = (QpcNow() - t0_) * fps_ / f + 1;
      continue;
    }
    if (!Encode(n)) {
      failed_ = true;
      break;
    }
    n++;
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
    auto tex = TextureOf(frame);
    D3D11_TEXTURE2D_DESC d;
    tex->GetDesc(&d);
    auto cs = frame.ContentSize();
    const UINT cw = std::min<UINT>(cs.Width, d.Width), ch = std::min<UINT>(cs.Height, d.Height);
    if (!vp_ || d.Width != vp_w_ || d.Height != vp_h_) CreateProcessor(d.Width, d.Height);

    CheckAv(av_hwframe_get_buffer(hw_frames_, out, 0), "get encoder surface");
    auto* out_view = OutputView(reinterpret_cast<ID3D11Texture2D*>(out->data[0]), UINT(intptr_t(out->data[1])));

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

void VideoCapture::DrawOverlay(InputOverlay& overlay) {
  if (!d2d_) {
    com_ptr<ID2D1Factory1> factory;
    D2D1_FACTORY_OPTIONS fo{};
    winrt::check_hresult(
        D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory1), &fo, factory.put_void()));
    com_ptr<ID2D1Device> d2d_dev;
    winrt::check_hresult(factory->CreateDevice(dev_.as<IDXGIDevice>().get(), d2d_dev.put()));
    winrt::check_hresult(d2d_dev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, d2d_.put()));
  }
  if (!d2d_target_) {
    const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                               D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    winrt::check_hresult(d2d_->CreateBitmapFromDxgiSurface(copy_.as<IDXGISurface>().get(), &props, d2d_target_.put()));
  }
  d2d_->SetTarget(d2d_target_.get());
  d2d_->BeginDraw();
  overlay.Draw(d2d_.get(), d2d_target_->GetSize());
  const HRESULT hr = d2d_->EndDraw();
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

ID3D11VideoProcessorOutputView* VideoCapture::OutputView(ID3D11Texture2D* tex, UINT slice) {
  if (tex != out_tex_) {  // FFmpeg keeps all encoder surfaces in one texture array
    out_views_.clear();
    out_tex_ = tex;
  }
  if (slice >= out_views_.size()) out_views_.resize(slice + 1);
  auto& v = out_views_[slice];
  if (!v) {
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC vd{};
    vd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2DARRAY;
    vd.Texture2DArray = {0, slice, 1};
    winrt::check_hresult(vdev_->CreateVideoProcessorOutputView(tex, venum_.get(), &vd, v.put()));
  }
  return v.get();
}
