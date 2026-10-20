#pragma once
#include <windows.h>

#include <cstdint>
#include <format>
#include <string>
#include <vector>

#include "log.h"

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

struct OverlayPreset {
  const char* name;
  std::vector<OverlayKey> keys;
};

inline const std::vector<OverlayPreset>& OverlayPresets() {
  static const std::vector<OverlayPreset> presets = {
      {"WASD and mouse",
       {{"Tab", "", 0, 0, 1.5f}, {"Q", "", 1.5f, 0}, {"W", "", 2.5f, 0}, {"E", "", 3.5f, 0}, {"R", "", 4.5f, 0},
        {"Shift", "", 0, 1, 1.5f}, {"A", "", 1.5f, 1}, {"S", "", 2.5f, 1}, {"D", "", 3.5f, 1}, {"F", "", 4.5f, 1},
        {"Ctrl", "", 0, 2, 1.5f}, {"Space", "", 1.5f, 2, 4}, {"LMB", "", 5.75f, 0}, {"RMB", "", 6.75f, 0},
        {"Mouse", "", 5.75f, 1, 2, 2}}},
      {"Full left hand and mouse",
       {{"Esc", "", 0, 0},          {"1", "", 1, 0},        {"2", "", 2, 0},         {"3", "", 3, 0},
        {"4", "", 4, 0},            {"5", "", 5, 0},        {"Tab", "", 0, 1, 1.5f}, {"Q", "", 1.5f, 1},
        {"W", "", 2.5f, 1},         {"E", "", 3.5f, 1},     {"R", "", 4.5f, 1},      {"T", "", 5.5f, 1},
        {"Caps", "", 0, 2, 1.75f},  {"A", "", 1.75f, 2},    {"S", "", 2.75f, 2},     {"D", "", 3.75f, 2},
        {"F", "", 4.75f, 2},        {"G", "", 5.75f, 2},    {"Shift", "", 0, 3, 2.25f}, {"Z", "", 2.25f, 3},
        {"X", "", 3.25f, 3},        {"C", "", 4.25f, 3},    {"V", "", 5.25f, 3},     {"Ctrl", "", 0, 4, 1.5f},
        {"Alt", "", 1.5f, 4, 1.25f}, {"Space", "", 2.75f, 4, 3.5f}, {"LMB", "", 7, 0}, {"RMB", "", 8, 0},
        {"Mouse", "", 7, 1, 2, 2},  {"WheelUp", "▲", 7, 3}, {"WheelDown", "▼", 8, 3}, {"Mouse4", "M4", 7, 4},
        {"Mouse5", "M5", 8, 4}}},
      {"Mouse only",
       {{"LMB", "", 0, 0, 1.5f}, {"RMB", "", 1.5f, 0, 1.5f}, {"Mouse", "", 0, 1, 3, 2},
        {"WheelUp", "▲", 0, 3, 1.5f}, {"WheelDown", "▼", 1.5f, 3, 1.5f}}},
      {"Keyboard only",
       {{"Tab", "", 0, 0, 1.5f}, {"Q", "", 1.5f, 0}, {"W", "", 2.5f, 0}, {"E", "", 3.5f, 0}, {"R", "", 4.5f, 0},
        {"Shift", "", 0, 1, 1.5f}, {"A", "", 1.5f, 1}, {"S", "", 2.5f, 1}, {"D", "", 3.5f, 1}, {"F", "", 4.5f, 1},
        {"Ctrl", "", 0, 2, 1.5f}, {"Space", "", 1.5f, 2, 4}}},
  };
  return presets;
}

struct OverlayConfig {
  bool enabled = false;
  std::string corner = "bottom-left";
  float scale = 1, opacity = 1;
  std::string mouse_mode = "wrap";  // "wrap": dot wraps at the pad's edges; "pan": dot stays centred, pad scrolls
  float mouse_sensitivity = 1, mouse_trail_length = 1, trail_width = 1;
  int mouse_trail_ms = 600;
  // Look. Colours are "#RRGGBB" or "#RRGGBBAA".
  std::string idle_color = "#00000073", pressed_color = "#FFFFFFE6", text_color = "#FFFFFF",
              pressed_text_color = "#000000", border_color = "#FFFFFF4D", trail_color = "#FFFFFF",
              click_color = "#FFCC40", font = "Segoe UI";
  float roundness = 0.15f;  // corner radius, in key widths
  std::vector<OverlayKey> keys = OverlayPresets()[0].keys;
  bool operator==(const OverlayConfig&) const = default;
};

struct Config {
  int clip_seconds = 30;
  std::string hotkey = "F8";  // one key, free of NVIDIA (Alt+F9/F10), Steam (F12) and Game Bar (Win+G)
  int fps = 60;
  int max_height = 1080;
  int bitrate_mbps = 20;
  // -7..0. Normal, not lowest: a game that keeps the GPU busy (uncapped fps) starves a lowest-priority
  // device, and the recording drops to a few fps. Our work is one small blit per frame either way.
  int gpu_priority = 0;
  std::string clips_folder;  // empty = <Videos>\Clip
  int min_free_gb = 2;
  bool record_game_audio = true;
  bool record_mic = true;
  std::vector<std::string> apps;  // extra apps whose audio is recorded, by exe
  std::vector<std::string> always_record, never_record;
  bool start_with_windows = true;
  bool copy_to_clipboard = true;     // each saved clip, as a file Ctrl+V pastes into Discord or Explorer
  bool save_notice = true;           // a "Clip saved" card over the game when a clip is saved
  std::string share_size = "discord";  // clipboard copies: "discord" (fits 10 MB) or "original"
  bool watermark = true;             // "Clipped with Clip" badge on exports and clipboard copies; recordings stay clean
  bool discord_presence = true;      // Discord activity: the game, "Clipping with Clip" and a link to the repo
  bool voice_save = false;  // "Hey Clip, save that" saves a clip; listens only while a game records
  bool share_caption = true;         // "#ClippedWithClip on <date>" pasted after a copied clip
  std::string double_tap = "RCtrl";  // second shortcut: this key tapped twice; "" = off
  OverlayConfig overlay;
};
