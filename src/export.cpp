#include "export.h"

#include <cmath>
#include <optional>

#include "gpu.h"
#include "watermark.h"

namespace {
AVPixelFormat PickD3D11(AVCodecContext*, const AVPixelFormat* fmts) {
  for (; *fmts != AV_PIX_FMT_NONE; fmts++)
    if (*fmts == AV_PIX_FMT_D3D11) return *fmts;
  return AV_PIX_FMT_NONE;
}

struct FrameDeleter {
  void operator()(AVFrame* f) const { av_frame_free(&f); }
};
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;

struct Job {
  AVFormatContext* in = nullptr;
  AVFormatContext* out = nullptr;
  AVCodecContext* dec = nullptr;
  AVCodecContext* enc = nullptr;
  AVBufferRef* hw_dev = nullptr;
  AVBufferRef* frames = nullptr;
  AVPacket* pkt = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();
  ~Job() {
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&enc);
    avcodec_free_context(&dec);
    av_buffer_unref(&frames);
    av_buffer_unref(&hw_dev);
    if (out) {
      avio_closep(&out->pb);
      avformat_free_context(out);
    }
    avformat_close_input(&in);
  }
};

// The kept parts of a clip, mapped onto the export's timeline.
struct Segments {
  std::vector<std::pair<double, double>> keep;  // sorted, merged
  double total = 0;
  Segments(std::vector<std::pair<double, double>> k, double duration) {
    if (k.empty()) k = {{0, duration}};
    for (auto& [a, b] : k) a = std::clamp(a, 0.0, duration), b = std::clamp(b, 0.0, duration);
    std::ranges::sort(k);
    for (auto& [a, b] : k) {
      if (b - a < 0.05) continue;
      if (!keep.empty() && a <= keep.back().second) keep.back().second = std::max(keep.back().second, b);
      else keep.emplace_back(a, b);
    }
    for (auto& [a, b] : keep) total += b - a;
  }
  // Export time for clip time t, or nothing if t is cut.
  std::optional<double> Map(double t) const {
    double before = 0;
    for (auto& [a, b] : keep) {
      if (t >= a && t < b) return before + t - a;
      before += b - a;
    }
    return std::nullopt;
  }
  double end() const { return keep.empty() ? 0 : keep.back().second; }
};

std::string Run(const ExportOptions& o, const std::function<void(double)>& progress, const std::atomic<bool>& cancel) {
  Job j;
  CheckAv(avformat_open_input(&j.in, Utf8(o.in.wstring()).c_str(), nullptr, nullptr), "open clip");
  CheckAv(avformat_find_stream_info(j.in, nullptr), "read clip");
  const int vi = av_find_best_stream(j.in, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  CheckAv(vi, "find video");
  int ai = -1;
  for (unsigned i = 0, n = 0; i < j.in->nb_streams && ai < 0 && o.audio >= 0; i++)
    if (j.in->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && int(n++) == o.audio) ai = int(i);
  AVStream* vs = j.in->streams[vi];
  AVStream* as = ai >= 0 ? j.in->streams[ai] : nullptr;

  const double duration = j.in->duration > 0 ? double(j.in->duration) / AV_TIME_BASE : 0;
  const Segments seg(o.keep, duration);
  if (seg.total < 0.1) return "nothing to export";

  auto dev = CreateVideoDevice(-7);
  j.hw_dev = FfmpegDevice(dev.dev.get());
  const AVCodec* dc = avcodec_find_decoder(vs->codecpar->codec_id);
  if (!dc) return "can't decode this clip";
  j.dec = avcodec_alloc_context3(dc);
  CheckAv(avcodec_parameters_to_context(j.dec, vs->codecpar), "read video format");
  j.dec->hw_device_ctx = av_buffer_ref(j.hw_dev);
  j.dec->get_format = PickD3D11;
  j.dec->pkt_timebase = vs->time_base;
  CheckAv(avcodec_open2(j.dec, dc, nullptr), "open decoder");

  // Source area (all of it, or a 9:16 slice), output size and bitrate. A size target leaves room for
  // the audio and the MP4 container.
  const int src_w = j.dec->width, src_h = j.dec->height;
  RECT src{0, 0, src_w, src_h};
  if (o.vertical) {
    const int cw = std::min(src_w, (src_h * 9 / 16) & ~1);
    src.left = LONG(std::clamp(o.crop_x, 0.0, 1.0) * (src_w - cw)) & ~1;
    src.right = src.left + cw;
  }
  int64_t bitrate = o.bitrate_mbps * int64_t(1000000);
  int out_h = o.max_height > 0 ? std::min(src_h, o.max_height) : src_h;
  if (o.target_bytes > 0) {
    const int64_t audio = !as ? 0 : as->codecpar->bit_rate > 0 ? as->codecpar->bit_rate : 160000;
    bitrate = std::clamp<int64_t>(int64_t(double(o.target_bytes) * 8 * 0.92 / seg.total) - audio, 300000, bitrate);
    if (bitrate < 2500000) out_h = std::min(out_h, 720);
    if (bitrate < 1000000) out_h = std::min(out_h, 480);
  }
  out_h &= ~1;
  const int out_w = int(int64_t(src.right - src.left) * out_h / src_h) & ~1;
  // The recorded rate (60), not the average: a clip that dropped frames averages less, and encoding
  // at that rate merges its runs of real frames, so the copy would play at a fraction of the fps.
  AVRational fps = av_guess_frame_rate(j.in, vs, nullptr);
  if (fps.num <= 0 || fps.den <= 0) fps = vs->avg_frame_rate.num ? vs->avg_frame_rate : AVRational{60, 1};
  j.frames = EncoderSurfaces(j.hw_dev, out_w, out_h, 16);
  std::string enc_name;
  j.enc = OpenH264(j.frames, out_w, out_h, fps, bitrate, int(std::lround(av_q2d(fps) * 2)), enc_name);
  Log("export {} {} part(s), {:.1f} s -> {}x{} {} kbit/s {}", Utf8(o.in.filename().wstring()), seg.keep.size(),
      seg.total, out_w, out_h, bitrate / 1000, enc_name);

  auto vdev = dev.dev.as<ID3D11VideoDevice>();
  auto vctx = dev.ctx.as<ID3D11VideoContext1>();
  D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
  cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
  cd.InputFrameRate = cd.OutputFrameRate = {UINT(fps.num), UINT(fps.den)};
  cd.InputWidth = UINT(src_w);
  cd.InputHeight = UINT(src_h);
  cd.OutputWidth = UINT(out_w);
  cd.OutputHeight = UINT(out_h);
  cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_QUALITY;
  com_ptr<ID3D11VideoProcessorEnumerator> venum;
  com_ptr<ID3D11VideoProcessor> vp;
  winrt::check_hresult(vdev->CreateVideoProcessorEnumerator(&cd, venum.put()));
  winrt::check_hresult(vdev->CreateVideoProcessor(venum.get(), 0, vp.put()));
  vctx->VideoProcessorSetStreamColorSpace1(vp.get(), 0, DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
  vctx->VideoProcessorSetOutputColorSpace1(vp.get(), DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
  vctx->VideoProcessorSetStreamFrameFormat(vp.get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
  vctx->VideoProcessorSetStreamAutoProcessingMode(vp.get(), 0, FALSE);
  vctx->VideoProcessorSetStreamSourceRect(vp.get(), 0, TRUE, &src);
  const RECT dst{0, 0, out_w, out_h};
  vctx->VideoProcessorSetStreamDestRect(vp.get(), 0, TRUE, &dst);
  std::map<std::pair<ID3D11Texture2D*, UINT>, com_ptr<ID3D11VideoProcessorInputView>> in_views;
  OutputViews out_views;
  std::optional<Watermark> wm;
  if (o.watermark) wm.emplace(dev.dev.get(), dev.ctx.get(), out_w, out_h);

  std::error_code ec;
  std::filesystem::create_directories(o.out.parent_path(), ec);
  CheckAv(avformat_alloc_output_context2(&j.out, nullptr, "mp4", nullptr), "make MP4");
  AVStream* ovs = avformat_new_stream(j.out, nullptr);
  CheckAv(avcodec_parameters_from_context(ovs->codecpar, j.enc), "video format");
  ovs->time_base = j.enc->time_base;
  AVStream* oas = nullptr;
  if (as) {
    oas = avformat_new_stream(j.out, nullptr);
    CheckAv(avcodec_parameters_copy(oas->codecpar, as->codecpar), "audio format");
    oas->codecpar->codec_tag = 0;
    oas->time_base = as->time_base;
  }
  const std::string out_path = Utf8(o.out.wstring());
  j.out->url = av_strdup(out_path.c_str());  // +faststart reopens the file by name
  CheckAv(avio_open(&j.out->pb, out_path.c_str(), AVIO_FLAG_WRITE), "create file");
  AVDictionary* mo = nullptr;
  av_dict_set(&mo, "movflags", "+faststart", 0);
  const int hr = avformat_write_header(j.out, &mo);
  av_dict_free(&mo);
  CheckAv(hr, "write MP4");

  CheckAv(av_seek_frame(j.in, -1, int64_t(seg.keep.front().first * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD), "seek");
  avcodec_flush_buffers(j.dec);
  const double vtb = av_q2d(vs->time_base), end = seg.end();
  int64_t last_pts = -1, last_audio = -1;
  bool done = false;
  int last_pct = -1;

  auto drain_encoder = [&] {
    while (avcodec_receive_packet(j.enc, j.pkt) == 0) {
      av_packet_rescale_ts(j.pkt, j.enc->time_base, ovs->time_base);
      j.pkt->stream_index = ovs->index;
      CheckAv(av_interleaved_write_frame(j.out, j.pkt), "write video");
    }
  };
  auto use_frame = [&](AVFrame* f) {
    const int64_t ts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
    if (done || ts == AV_NOPTS_VALUE) return;
    const double t = double(ts) * vtb;
    if (t >= end) {
      done = true;
      return;
    }
    const auto at = seg.Map(t);
    if (!at) return;
    const int64_t pts = std::llround(*at * av_q2d(av_inv_q(j.enc->time_base)));
    if (pts <= last_pts) return;
    last_pts = pts;
    FramePtr out(av_frame_alloc());
    CheckAv(av_hwframe_get_buffer(j.frames, out.get(), 0), "get encoder surface");
    auto* in_tex = reinterpret_cast<ID3D11Texture2D*>(f->data[0]);
    const UINT in_slice = UINT(intptr_t(f->data[1]));
    auto& iv = in_views[{in_tex, in_slice}];
    if (!iv) {
      D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC vd{};
      vd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
      vd.Texture2D.ArraySlice = in_slice;
      winrt::check_hresult(vdev->CreateVideoProcessorInputView(in_tex, venum.get(), &vd, iv.put()));
    }
    auto* out_tex = reinterpret_cast<ID3D11Texture2D*>(out->data[0]);
    const UINT out_slice = UINT(intptr_t(out->data[1]));
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = iv.get();
    winrt::check_hresult(vctx->VideoProcessorBlt(
        vp.get(), OutputViewFor(out_views, vdev.get(), venum.get(), out_tex, out_slice), 0, 1, &stream));
    if (wm) wm->Draw(out_tex, out_slice, *at);
    out->pts = pts;
    CheckAv(avcodec_send_frame(j.enc, out.get()), "encode");
    drain_encoder();
    const int pct = int(100 * *at / seg.total);
    if (pct != last_pct) progress((last_pct = pct) / 100.0);
  };
  auto drain_decoder = [&] {
    while (avcodec_receive_frame(j.dec, j.frame) == 0) {
      use_frame(j.frame);
      av_frame_unref(j.frame);
    }
  };

  while (!done && !cancel) {
    if (av_read_frame(j.in, j.pkt) < 0) break;
    if (j.pkt->stream_index == vi) {
      CheckAv(avcodec_send_packet(j.dec, j.pkt), "decode");
      av_packet_unref(j.pkt);
      drain_decoder();
      continue;
    }
    std::optional<double> at;
    if (oas && j.pkt->stream_index == ai && j.pkt->pts != AV_NOPTS_VALUE)
      at = seg.Map(double(j.pkt->pts) * av_q2d(as->time_base));
    const int64_t pts = at ? std::llround(*at / av_q2d(as->time_base)) : -1;
    if (!at || pts <= last_audio) {
      av_packet_unref(j.pkt);
      continue;
    }
    last_audio = pts;
    j.pkt->pts = j.pkt->dts = pts;
    av_packet_rescale_ts(j.pkt, as->time_base, oas->time_base);
    j.pkt->stream_index = oas->index;
    j.pkt->pos = -1;
    CheckAv(av_interleaved_write_frame(j.out, j.pkt), "write audio");
  }
  if (cancel) return "cancelled";
  avcodec_send_packet(j.dec, nullptr);
  drain_decoder();
  CheckAv(avcodec_send_frame(j.enc, nullptr), "finish encoding");
  drain_encoder();
  CheckAv(av_write_trailer(j.out), "finish MP4");
  progress(1);
  return "";
}
}  // namespace

std::string ExportClip(const ExportOptions& o, const std::function<void(double)>& progress,
                       const std::atomic<bool>& cancel) {
  std::string err;
  try {
    err = Run(o, progress, cancel);
  } catch (const winrt::hresult_error& e) {
    err = winrt::to_string(e.message());
  } catch (const std::exception& e) {
    err = e.what();
  }
  if (!err.empty()) {
    std::error_code ec;
    std::filesystem::remove(o.out, ec);
    if (err != "cancelled") Log("export failed: {}", err);
  }
  return err;
}

ClipInfo ReadClipInfo(const std::filesystem::path& clip) {
  ClipInfo info;
  AVFormatContext* in = nullptr;
  AVCodecContext* dec = nullptr;
  AVPacket* pkt = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();
  try {
    CheckAv(avformat_open_input(&in, Utf8(clip.wstring()).c_str(), nullptr, nullptr), "open clip");
    CheckAv(avformat_find_stream_info(in, nullptr), "read clip");
    info.duration = in->duration > 0 ? double(in->duration) / AV_TIME_BASE : 0;
    int ai = -1;
    for (unsigned i = 0; i < in->nb_streams; i++) {
      AVStream* st = in->streams[i];
      if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && !info.width)
        info.width = st->codecpar->width, info.height = st->codecpar->height;
      if (st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
      const AVDictionaryEntry* name = av_dict_get(st->metadata, "handler_name", nullptr, 0);
      info.tracks.push_back(name ? name->value : std::format("Track {}", info.tracks.size() + 1));
      if (ai < 0) ai = int(i);
    }
    if (ai >= 0) {
      AVStream* as = in->streams[ai];
      const AVCodec* c = avcodec_find_decoder(as->codecpar->codec_id);
      dec = avcodec_alloc_context3(c);
      CheckAv(avcodec_parameters_to_context(dec, as->codecpar), "audio format");
      CheckAv(avcodec_open2(dec, c, nullptr), "open audio decoder");
      for (unsigned i = 0; i < in->nb_streams; i++) in->streams[i]->discard = int(i) == ai ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
      info.peaks.assign(size_t(std::ceil(info.duration * kPeaksPerSecond)) + 1, 0.f);
      auto take = [&] {
        while (avcodec_receive_frame(dec, frame) == 0) {
          const double t0 = frame->pts != AV_NOPTS_VALUE ? double(frame->pts) * av_q2d(as->time_base) : 0;
          const int ch = frame->ch_layout.nb_channels;
          for (int s = 0; s < frame->nb_samples; s++) {
            float v = 0;
            for (int k = 0; k < ch; k++) {
              float x = 0;
              switch (frame->format) {
                case AV_SAMPLE_FMT_FLTP: x = reinterpret_cast<float*>(frame->data[k])[s]; break;
                case AV_SAMPLE_FMT_FLT: x = reinterpret_cast<float*>(frame->data[0])[s * ch + k]; break;
                case AV_SAMPLE_FMT_S16P: x = reinterpret_cast<int16_t*>(frame->data[k])[s] / 32768.f; break;
                case AV_SAMPLE_FMT_S16: x = reinterpret_cast<int16_t*>(frame->data[0])[s * ch + k] / 32768.f; break;
                default: break;
              }
              v = std::max(v, std::abs(x));
            }
            const size_t b = size_t((t0 + double(s) / frame->sample_rate) * kPeaksPerSecond);
            if (b < info.peaks.size()) info.peaks[b] = std::max(info.peaks[b], std::min(v, 1.f));
          }
          av_frame_unref(frame);
        }
      };
      while (av_read_frame(in, pkt) >= 0) {
        if (pkt->stream_index == ai && avcodec_send_packet(dec, pkt) >= 0) take();
        av_packet_unref(pkt);
      }
      avcodec_send_packet(dec, nullptr);
      take();
    }
  } catch (const std::exception& e) {
    info.error = e.what();
  }
  av_frame_free(&frame);
  av_packet_free(&pkt);
  avcodec_free_context(&dec);
  avformat_close_input(&in);
  return info;
}
