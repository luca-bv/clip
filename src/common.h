#pragma once
#include <windows.h>

#include <cstdint>
#include <format>
#include <string>
#include <vector>

void LogLine(const std::string& line);
template <class... A>
void Log(std::format_string<A...> f, A&&... a) {
  LogLine(std::format(f, std::forward<A>(a)...));
}

std::string Utf8(std::wstring_view w);
std::wstring Wide(std::string_view s);

// ASCII lowercase; exe names are compared lowercased everywhere.
inline std::string Lower(std::string s) {
  for (auto& c : s) c = (c >= 'A' && c <= 'Z') ? char(c + 32) : c;
  return s;
}
inline std::string Stem(const std::string& exe) {
  return exe.ends_with(".exe") ? exe.substr(0, exe.size() - 4) : exe;
}

inline int64_t QpcNow() {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return t.QuadPart;
}
inline int64_t QpcFreq() {
  static const int64_t f = [] {
    LARGE_INTEGER t;
    QueryPerformanceFrequency(&t);
    return t.QuadPart;
  }();
  return f;
}
// QPC ticks -> units of 1/rate s, without overflowing on long sessions.
inline int64_t QpcTo(int64_t ticks, int64_t rate) {
  const int64_t f = QpcFreq();
  return ticks / f * rate + ticks % f * rate / f;
}

struct OverlayKey {
  std::string key, label;            // label "" = key name; key "Mouse" = mouse movement pad
  float x = 0, y = 0, w = 1, h = 1;  // in key widths from the overlay's top-left
  bool operator==(const OverlayKey&) const = default;
};

struct OverlayConfig {
  bool enabled = false;
  std::string corner = "bottom-left";
  float scale = 1, opacity = 0.85f;
  std::string mouse_mode = "wrap";  // "wrap": dot wraps at the pad's edges; "pan": dot stays centred, pad scrolls
  float mouse_sensitivity = 1, mouse_trail_length = 1;
  int mouse_trail_ms = 600;
  std::vector<OverlayKey> keys = {
      {"Tab", "", 0, 0, 1.5f},    {"Q", "", 1.5f, 0},  {"W", "", 2.5f, 0},     {"E", "", 3.5f, 0},
      {"R", "", 4.5f, 0},         {"Shift", "", 0, 1, 1.5f}, {"A", "", 1.5f, 1}, {"S", "", 2.5f, 1},
      {"D", "", 3.5f, 1},         {"F", "", 4.5f, 1},  {"Ctrl", "", 0, 2, 1.5f}, {"Space", "", 1.5f, 2, 4},
      {"LMB", "", 5.75f, 0},      {"RMB", "", 6.75f, 0}, {"Mouse", "", 5.75f, 1, 2, 2},
  };
  bool operator==(const OverlayConfig&) const = default;
};

struct Config {
  int clip_seconds = 30;
  std::string hotkey = "Alt+F9";
  int fps = 60;
  int max_height = 1080;
  int bitrate_mbps = 20;
  std::string clips_folder;  // empty = <Videos>\Clip
  int min_free_gb = 2;
  bool record_game_audio = true;
  bool record_mic = true;
  std::vector<std::string> apps;  // extra apps whose audio is recorded, by exe
  std::vector<std::string> always_record, never_record;
  bool start_with_windows = true;
  OverlayConfig overlay;
};
