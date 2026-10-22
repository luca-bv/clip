#pragma once
// Encoded packets of the last N seconds, and cutting them into an MP4. Only FFmpeg and std, so it's
// tested on any OS (tests/ring_test.cpp).
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "log.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

inline void CheckAv(int r, const char* what) {
  if (r >= 0) return;
  char buf[AV_ERROR_MAX_STRING_SIZE];
  av_strerror(r, buf, sizeof buf);
  throw std::runtime_error(std::format("{}: {}", what, buf));
}

// Opens a clip as MP4 and from disk only, so a file someone drops in the clips folder reaches no other demuxer.
inline int OpenClip(AVFormatContext** fmt, const char* path) {
  AVDictionary* o = nullptr;
  av_dict_set(&o, "protocol_whitelist", "file", 0);
  const int r = avformat_open_input(fmt, path, av_find_input_format("mp4"), &o);
  av_dict_free(&o);
  return r;
}

// Encoded packets of every track, trimmed to the clip length. Times are relative to session start.
class PacketRing {
 public:
  ~PacketRing();
  int AddTrack(std::string title, const AVCodecContext* enc, bool video);
  void SetKeepSeconds(int s);
  // Moves every finished packet out of `enc` into `track`.
  void Drain(int track, AVCodecContext* enc);
  // Writes [end_us - seconds, end_us] to an MP4. Returns "" or an error.
  std::string Export(int64_t end_us, int seconds, const std::filesystem::path& file);

 private:
  struct Track {
    std::string title;
    AVCodecParameters* par;
    AVRational tb;
    bool video;
    std::deque<AVPacket*> pkts;
  };
  static int64_t Us(const Track& t, int64_t ts) { return av_rescale_q(ts, t.tb, {1, 1000000}); }
  void Evict(int64_t now_us);

  std::mutex m_;
  std::vector<std::unique_ptr<Track>> tracks_;
  int64_t keep_us_ = 0;
};

