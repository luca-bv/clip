#include "gpu.h"

#include <dxgi1_6.h>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}

VideoDevice CreateVideoDevice(int gpu_priority) {
  com_ptr<IDXGIFactory6> factory;
  winrt::check_hresult(CreateDXGIFactory2(0, IID_PPV_ARGS(factory.put())));
  com_ptr<IDXGIAdapter1> adapter;
  winrt::check_hresult(
      factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(adapter.put())));
  DXGI_ADAPTER_DESC1 desc;
  adapter->GetDesc1(&desc);
  Log("GPU: {}", Utf8(desc.Description));

  VideoDevice d;
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  winrt::check_hresult(D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                         D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                         levels, 2, D3D11_SDK_VERSION, d.dev.put(), nullptr, d.ctx.put()));
  d.dev.as<ID3D11Multithread>()->SetMultithreadProtected(TRUE);
  if (HRESULT hr = d.dev.as<IDXGIDevice>()->SetGPUThreadPriority(gpu_priority); FAILED(hr))
    Log("GPU priority {} not set (0x{:08x})", gpu_priority, uint32_t(hr));
  return d;
}

AVBufferRef* FfmpegDevice(ID3D11Device* dev) {
  AVBufferRef* hw = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
  static_cast<AVD3D11VADeviceContext*>(reinterpret_cast<AVHWDeviceContext*>(hw->data)->hwctx)->device = dev;
  dev->AddRef();  // FFmpeg releases it
  if (int r = av_hwdevice_ctx_init(hw); r < 0) {
    av_buffer_unref(&hw);
    CheckAv(r, "init D3D11 for FFmpeg");
  }
  return hw;
}

AVBufferRef* EncoderSurfaces(AVBufferRef* hw_dev, int w, int h, int pool) {
  auto make = [&](int size) {
    AVBufferRef* frames = av_hwframe_ctx_alloc(hw_dev);
    auto* fc = reinterpret_cast<AVHWFramesContext*>(frames->data);
    fc->format = AV_PIX_FMT_D3D11;
    fc->sw_format = AV_PIX_FMT_NV12;
    fc->width = w;
    fc->height = h;
    fc->initial_pool_size = size;
    static_cast<AVD3D11VAFramesContext*>(fc->hwctx)->BindFlags = D3D11_BIND_RENDER_TARGET;
    if (av_hwframe_ctx_init(frames) < 0) av_buffer_unref(&frames);
    return frames;
  };
  if (AVBufferRef* frames = make(pool)) return frames;
  // Some NVIDIA drivers refuse NV12 render-target arrays over 2 slices; FFmpeg then makes one texture per frame.
  Log("encoder surface array refused; using separate textures");
  if (AVBufferRef* frames = make(0)) return frames;
  throw std::runtime_error("create encoder surfaces: the GPU refused NV12 render targets");
}

AVCodecContext* OpenH264(AVBufferRef* frames, int w, int h, AVRational fps, int64_t bitrate, int gop,
                         std::string& name) {
  for (std::string_view n : {"h264_nvenc", "h264_amf", "h264_qsv"}) {
    const AVCodec* codec = avcodec_find_encoder_by_name(n.data());
    if (!codec) continue;
    AVCodecContext* e = avcodec_alloc_context3(codec);
    e->width = w;
    e->height = h;
    e->time_base = av_inv_q(fps);
    e->framerate = fps;
    e->pix_fmt = AV_PIX_FMT_D3D11;
    e->sw_pix_fmt = AV_PIX_FMT_NV12;
    e->hw_frames_ctx = av_buffer_ref(frames);
    e->bit_rate = e->rc_max_rate = bitrate;
    e->rc_buffer_size = int(std::min<int64_t>(bitrate, INT_MAX));  // 1 s VBV
    e->gop_size = gop;
    e->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    e->color_range = AVCOL_RANGE_MPEG;
    e->colorspace = AVCOL_SPC_BT709;
    e->color_primaries = AVCOL_PRI_BT709;
    e->color_trc = AVCOL_TRC_BT709;
    AVDictionary* o = nullptr;
    // Frames sent as AV_PICTURE_TYPE_I become IDR, which a clip can start on; else only an I-frame.
    if (n == "h264_nvenc") {
      av_dict_set(&o, "preset", "p4", 0);
      av_dict_set(&o, "tune", "hq", 0);
      av_dict_set(&o, "rc", "cbr", 0);
      av_dict_set(&o, "forced-idr", "1", 0);
    } else if (n == "h264_amf") {
      av_dict_set(&o, "rc", "cbr", 0);
      av_dict_set(&o, "quality", "balanced", 0);
      av_dict_set(&o, "forced_idr", "1", 0);
    } else {
      av_dict_set(&o, "forced_idr", "1", 0);
    }
    const int r = avcodec_open2(e, codec, &o);
    av_dict_free(&o);
    if (r >= 0) {
      name = n;
      return e;
    }
    Log("encoder {} unavailable", n);
    avcodec_free_context(&e);
  }
  throw std::runtime_error("no hardware H.264 encoder found on this GPU");
}

ID3D11VideoProcessorOutputView* OutputViewFor(OutputViews& cache, ID3D11VideoDevice* vdev,
                                              ID3D11VideoProcessorEnumerator* venum, ID3D11Texture2D* tex, UINT slice) {
  auto& v = cache[{tex, slice}];
  if (!v) {
    D3D11_TEXTURE2D_DESC d;
    tex->GetDesc(&d);
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC vd{};
    if (d.ArraySize > 1) {
      vd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2DARRAY;
      vd.Texture2DArray = {0, slice, 1};
    } else {
      vd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    }
    winrt::check_hresult(vdev->CreateVideoProcessorOutputView(tex, venum, &vd, v.put()));
  }
  return v.get();
}
