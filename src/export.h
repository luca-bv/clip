#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

struct ExportOptions {
  std::filesystem::path in, out;
  std::vector<std::pair<double, double>> keep;  // seconds of the clip to keep, joined in order; empty = all
  int max_height = 0;         // 0 = the clip's own height
  int64_t target_bytes = 0;   // fit the file in this size (e.g. Discord's limit); 0 = use bitrate_mbps
  int bitrate_mbps = 20;
  bool watermark = true;
  int audio = 0;              // which audio track to keep (0 = the mix of everything), -1 = none
  // When set, overrides `audio`: a volume per separate track (gains[0] for track 1, the game, gains[1] for
  // track 2, the mic...), 0 = left out, 1 = as recorded. The kept tracks are mixed into one.
  std::vector<float> gains;
  bool vertical = false;      // crop to 9:16 for phones
  double crop_x = 0.5;        // where the vertical crop sits, 0 = left edge, 1 = right edge
};

// Cuts, scales and watermarks a clip on the GPU (hardware decode, video processor, watermark, hardware
// encode) and copies one audio track as is, or mixes the separate tracks at their own volumes. Runs at the lowest GPU priority so a game keeps its frames.
// Returns "" or what went wrong; a failed or cancelled export leaves no file.
std::string ExportClip(const ExportOptions& o, const std::function<void(double)>& progress,
                       const std::atomic<bool>& cancel);

struct ClipInfo {
  double duration = 0;
  int width = 0, height = 0;
  std::vector<std::string> tracks;  // audio track names, in order
  std::vector<float> peaks;         // loudness of the first audio track, kPeaksPerSecond per second, 0..1
  std::string error;
};
inline constexpr int kPeaksPerSecond = 50;
ClipInfo ReadClipInfo(const std::filesystem::path& clip);
