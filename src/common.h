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
};
