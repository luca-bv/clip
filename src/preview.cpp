#include "preview.h"

#include <cmath>
#include <fstream>

#include "common.h"
#include "ring.h"

extern "C" {
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace {
struct Decoder {
  AVFormatContext* fmt = nullptr;
  AVCodecContext* dec = nullptr;
  AVPacket* pkt = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();
  int vi = -1;
  ~Decoder() {
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
  }
};

// A YUV 4:2:0 picture for the JPEG encoder; tiles are scaled straight into their spot in it.
struct Picture {
  AVFrame* f = av_frame_alloc();
  Picture(int w, int h) {
    f->format = AV_PIX_FMT_YUVJ420P;
    f->width = w;
    f->height = h;
    av_frame_get_buffer(f, 32);
    for (int y = 0; y < h; y++) memset(f->data[0] + y * f->linesize[0], 16, w);
    for (int p = 1; p < 3; p++)
      for (int y = 0; y < h / 2; y++) memset(f->data[p] + y * f->linesize[p], 128, w / 2);
  }
  ~Picture() { av_frame_free(&f); }
};

void Blit(const AVFrame* src, Picture& dst, int x, int y, int w, int h, SwsContext*& sws) {
  // Plain YUV420P at full range is YUVJ420P's layout without swscale's deprecation warning per tile.
  sws = sws_getCachedContext(sws, src->width, src->height, AVPixelFormat(src->format), w, h, AV_PIX_FMT_YUV420P,
                             SWS_BILINEAR, nullptr, nullptr, nullptr);
  if (!sws) return;
  const int cs = src->colorspace == AVCOL_SPC_BT709 ? SWS_CS_ITU709 : SWS_CS_ITU601;
  sws_setColorspaceDetails(sws, sws_getCoefficients(cs), src->color_range == AVCOL_RANGE_JPEG,
                           sws_getCoefficients(SWS_CS_ITU601), 1, 0, 1 << 16, 1 << 16);
  uint8_t* d[4] = {dst.f->data[0] + y * dst.f->linesize[0] + x,
                   dst.f->data[1] + (y / 2) * dst.f->linesize[1] + x / 2,
                   dst.f->data[2] + (y / 2) * dst.f->linesize[2] + x / 2, nullptr};
  sws_scale(sws, src->data, src->linesize, 0, src->height, d, dst.f->linesize);
}

void WriteJpeg(Picture& pic, const std::filesystem::path& file) {
  const AVCodec* c = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
  if (!c) throw std::runtime_error("no JPEG encoder");
  AVCodecContext* e = avcodec_alloc_context3(c);
  e->width = pic.f->width;
  e->height = pic.f->height;
  e->pix_fmt = AV_PIX_FMT_YUVJ420P;
  e->time_base = {1, 1};
  e->flags |= AV_CODEC_FLAG_QSCALE;
  e->global_quality = FF_QP2LAMBDA * 4;
  AVPacket* p = av_packet_alloc();
  std::string err;
  if (avcodec_open2(e, c, nullptr) < 0) err = "open JPEG encoder";
  else if (avcodec_send_frame(e, pic.f) < 0 || avcodec_send_frame(e, nullptr) < 0 || avcodec_receive_packet(e, p) < 0)
    err = "encode JPEG";
  else {
    const auto tmp = std::filesystem::path(file).concat(L".part");
    std::ofstream(tmp, std::ios::binary | std::ios::trunc).write(reinterpret_cast<const char*>(p->data), p->size);
    std::error_code ec;
    std::filesystem::rename(tmp, file, ec);
  }
  av_packet_free(&p);
  avcodec_free_context(&e);
  if (!err.empty()) throw std::runtime_error(err);
}
}  // namespace

std::string MakePreview(const std::filesystem::path& clip, const std::filesystem::path& dir) {
  try {
    Decoder d;
    CheckAv(avformat_open_input(&d.fmt, Utf8(clip.wstring()).c_str(), nullptr, nullptr), "open clip");
    CheckAv(avformat_find_stream_info(d.fmt, nullptr), "read clip");
    d.vi = av_find_best_stream(d.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    CheckAv(d.vi, "find video");
    for (unsigned i = 0; i < d.fmt->nb_streams; i++) d.fmt->streams[i]->discard = int(i) == d.vi ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
    const AVStream* st = d.fmt->streams[d.vi];
    const AVCodec* c = avcodec_find_decoder(st->codecpar->codec_id);
    if (!c) return "can't decode this clip";
    d.dec = avcodec_alloc_context3(c);
    CheckAv(avcodec_parameters_to_context(d.dec, st->codecpar), "video format");
    d.dec->thread_count = 2;  // background work: leave the cores to the game
    d.dec->thread_type = FF_THREAD_SLICE;  // frame threading would hold each keyframe back until the next one
    d.dec->skip_frame = AVDISCARD_NONKEY;
    d.dec->skip_loop_filter = AVDISCARD_ALL;  // deblocking is invisible at thumbnail size
    CheckAv(avcodec_open2(d.dec, c, nullptr), "open decoder");
    const double dur = d.fmt->duration > 0 ? double(d.fmt->duration) / AV_TIME_BASE : 0;
    if (dur <= 0) return "clip has no length";

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    SwsContext* sws = nullptr;
    Picture thumb(480, 270);
    Picture strip(kStripCols * kTileW, (kStripTiles + kStripCols - 1) / kStripCols * kTileH);
    // One pass over the keyframes, each decoded once. A tile shows the last keyframe at or before its time.
    const double thumb_at = std::min(1.0, dur / 2), tb = av_q2d(st->time_base);
    int tile = 0, keys = 0;
    bool thumbed = false;
    AVFrame* prev = nullptr;
    auto fill = [&](double upto) {  // tiles (and the thumbnail) whose time comes before `upto` get `prev`
      if (!prev) return;
      for (; tile < kStripTiles && (tile + 0.5) * dur / kStripTiles < upto; tile++)
        Blit(prev, strip, tile % kStripCols * kTileW, tile / kStripCols * kTileH, kTileW, kTileH, sws);
      if (!thumbed && thumb_at < upto) Blit(prev, thumb, 0, 0, 480, 270, sws), thumbed = true;
    };
    auto take = [&] {
      while (avcodec_receive_frame(d.dec, d.frame) == 0) {
        const int64_t ts = d.frame->best_effort_timestamp != AV_NOPTS_VALUE ? d.frame->best_effort_timestamp : d.frame->pts;
        fill(double(ts) * tb);
        av_frame_free(&prev);
        prev = av_frame_clone(d.frame);
        keys++;
        av_frame_unref(d.frame);
      }
    };
    while (av_read_frame(d.fmt, d.pkt) >= 0) {
      if (d.pkt->stream_index == d.vi && (d.pkt->flags & AV_PKT_FLAG_KEY) && avcodec_send_packet(d.dec, d.pkt) >= 0) take();
      av_packet_unref(d.pkt);
    }
    avcodec_send_packet(d.dec, nullptr);
    take();
    fill(1e300);
    av_frame_free(&prev);
    WriteJpeg(thumb, dir / L"thumb.jpg");
    std::ofstream(dir / L"duration.txt") << dur << " " << keys;  // distinct pictures in the strip
    WriteJpeg(strip, dir / L"strip.jpg");  // last: its presence means the previews are done
    sws_freeContext(sws);
    return "";
  } catch (const std::exception& e) {
    return e.what();
  }
}
