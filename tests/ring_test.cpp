// Saving a clip from the packet ring: GOP-aligned start, eviction, A/V alignment, a track that only
// exists for part of the buffer, and an audio encoder restarted mid-track (source toggled off and on).
// Real encoders (mpeg4 with B-frames, AAC) and a real MP4, probed back.
#undef NDEBUG  // asserts are the test
#include <cassert>
#include <cmath>
#include <cstdio>

#include "../src/ring.h"

void LogLine(const std::string& line) { std::printf("%s\n", line.c_str()); }

namespace {
constexpr int kFps = 30, kRate = 48000, kFrame = 1024;

AVCodecContext* OpenVideo() {
  const AVCodec* c = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
  AVCodecContext* e = avcodec_alloc_context3(c);
  e->width = e->height = 64;
  e->pix_fmt = AV_PIX_FMT_YUV420P;
  e->time_base = {1, kFps};
  e->framerate = {kFps, 1};
  e->gop_size = kFps;
  e->max_b_frames = 2;
  e->flags |= AV_CODEC_FLAG_GLOBAL_HEADER | AV_CODEC_FLAG_CLOSED_GOP;
  AVDictionary* o = nullptr;
  av_dict_set(&o, "sc_threshold", "1000000000", 0);  // closed GOPs need scene-cut detection off
  CheckAv(avcodec_open2(e, c, &o), "open mpeg4");
  av_dict_free(&o);
  return e;
}

AVCodecContext* OpenAudio() {
  const AVCodec* c = avcodec_find_encoder(AV_CODEC_ID_AAC);
  AVCodecContext* e = avcodec_alloc_context3(c);
  e->sample_fmt = AV_SAMPLE_FMT_FLTP;
  e->sample_rate = kRate;
  av_channel_layout_default(&e->ch_layout, 2);
  e->time_base = {1, kRate};
  e->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  CheckAv(avcodec_open2(e, c, nullptr), "open aac");
  return e;
}

void SendVideo(PacketRing& ring, int track, AVCodecContext* e, int64_t n) {
  AVFrame* f = av_frame_alloc();
  f->format = e->pix_fmt;
  f->width = e->width;
  f->height = e->height;
  CheckAv(av_frame_get_buffer(f, 0), "frame");
  for (int p = 0; p < 3; p++)
    for (int y = 0; y < (p ? 32 : 64); y++) std::fill_n(f->data[p] + y * f->linesize[p], p ? 32 : 64, uint8_t(n * 7));
  f->pts = n;
  CheckAv(avcodec_send_frame(e, f), "send video");
  av_frame_free(&f);
  ring.Drain(track, e);
}

void SendAudio(PacketRing& ring, int track, AVCodecContext* e, int64_t k) {
  AVFrame* f = av_frame_alloc();
  f->format = e->sample_fmt;
  f->nb_samples = kFrame;
  f->sample_rate = kRate;
  av_channel_layout_copy(&f->ch_layout, &e->ch_layout);
  CheckAv(av_frame_get_buffer(f, 0), "frame");
  for (int c = 0; c < 2; c++)
    for (int i = 0; i < kFrame; i++)
      reinterpret_cast<float*>(f->data[c])[i] = 0.2f * float(std::sin((k * kFrame + i) * 0.05));
  f->pts = k * kFrame;
  CheckAv(avcodec_send_frame(e, f), "send audio");
  av_frame_free(&f);
  ring.Drain(track, e);
}

struct Probe {
  int streams = 0;
  double duration = 0, video_start = 0, audio_start = 0, video_len = 0, audio_len = 0;
};

Probe ProbeFile(const std::string& path) {
  AVFormatContext* ic = nullptr;
  CheckAv(avformat_open_input(&ic, path.c_str(), nullptr, nullptr), "open clip");
  CheckAv(avformat_find_stream_info(ic, nullptr), "probe clip");
  Probe p{int(ic->nb_streams), ic->duration / double(AV_TIME_BASE)};
  for (unsigned i = 0; i < ic->nb_streams; i++) {
    const AVStream* st = ic->streams[i];
    const double start = st->start_time * av_q2d(st->time_base), len = st->duration * av_q2d(st->time_base);
    if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) p.video_start = start, p.video_len = len;
    else if (i == 1) p.audio_start = start, p.audio_len = len;  // the mix track
  }
  avformat_close_input(&ic);
  return p;
}
}  // namespace

int main() {
  av_log_set_level(AV_LOG_ERROR);
  const auto dir = std::filesystem::temp_directory_path() / "clip_ring_test";
  std::filesystem::create_directories(dir);

  PacketRing ring;
  ring.SetKeepSeconds(5);  // keeps 7 s
  AVCodecContext* video = OpenVideo();
  AVCodecContext* mix = OpenAudio();
  AVCodecContext* mic = nullptr;
  const int vt = ring.AddTrack("Video", video, true), mt = ring.AddTrack("Mix", mix, false);
  int mic_track = -1;

  // 20 s of video with audio interleaved by time; the mic exists from 12 s to 16 s and restarts at 14 s.
  int64_t k = 0;
  bool restarted = false;
  for (int64_t n = 0; n < 20 * kFps; n++) {
    SendVideo(ring, vt, video, n);
    for (; k * kFrame * kFps <= n * kRate; k++) {
      const double t = double(k * kFrame) / kRate;
      SendAudio(ring, mt, mix, k);
      const bool want_mic = t >= 12 && t < 16, restart = mic && t >= 14 && !restarted;
      if (want_mic && (!mic || restart)) {  // a new encoder on the same track, like toggling the mic off and on
        restarted = restarted || restart;
        avcodec_free_context(&mic);
        mic = OpenAudio();
        if (mic_track < 0) mic_track = ring.AddTrack("Microphone", mic, false);
      }
      if (!want_mic) avcodec_free_context(&mic);
      if (mic) SendAudio(ring, mic_track, mic, k);
    }
  }

  {  // last 5 s: starts on the keyframe at 15 s, all three tracks, audio lined up with video
    const auto path = (dir / "a.mp4").string();
    assert(ring.Export(20'000'000, 5, path).empty());
    const Probe p = ProbeFile(path);
    std::printf("5 s clip: %d streams, %.3f s, video %.3f+%.3f, audio %.3f+%.3f\n", p.streams, p.duration,
                p.video_start, p.video_len, p.audio_start, p.audio_len);
    assert(p.streams == 3);
    assert(p.duration > 4.8 && p.duration < 5.3);
    assert(std::abs(p.video_start - p.audio_start) < 0.05);
    assert(std::abs(p.video_len - p.audio_len) < 0.15);
  }
  {  // asking for more than is buffered gives what's kept (5 s + 2 s margin, from a keyframe)
    const auto path = (dir / "b.mp4").string();
    assert(ring.Export(20'000'000, 300, path).empty());
    const Probe p = ProbeFile(path);
    std::printf("300 s ask: %d streams, %.3f s\n", p.streams, p.duration);
    assert(p.duration > 6.8 && p.duration < 8.2);
  }
  {  // a window the mic wasn't in has no mic track
    const auto path = (dir / "c.mp4").string();
    assert(ring.Export(20'000'000, 3, path).empty());
    const Probe p = ProbeFile(path);
    std::printf("3 s clip: %d streams, %.3f s\n", p.streams, p.duration);
    assert(p.streams == 2 && p.duration > 2.8 && p.duration < 3.3);
  }
  avcodec_free_context(&video);
  avcodec_free_context(&mix);
  avcodec_free_context(&mic);
  std::filesystem::remove_all(dir);
  std::puts("ring ok");
}
