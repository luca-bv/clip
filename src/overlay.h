#pragma once
#include "common.h"

#include <unknwn.h>

#include <d2d1_1.h>
#include <dwrite.h>
#include <winrt/base.h>

#include <atomic>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "input.h"
#include "trail.h"

// Mouse counts to key widths for a pad of size 1 (300x200 px pads cross in ~6000 counts), and the
// default trail length as a fraction of the pad.
inline constexpr float kMouseCountsToPad = 0.000204f;
inline constexpr float kTrailToPad = 0.612f;

// Key names a layout can use, in menu order. "Mouse" is the movement pad.
const std::vector<std::string>& OverlayKeyNames();
std::string OverlayKeyName(UINT vk);             // "" if the overlay can't show it
std::string CanonicalKey(const std::string& s);  // "w" -> "W", "" if unknown
D2D1_COLOR_F ParseColor(const std::string& hex, D2D1_COLOR_F fallback);
std::string ColorHex(D2D1_COLOR_F c);

// What's held right now.
struct OverlayInput {
  std::set<std::string> down;
  std::map<std::string, int64_t> lit_until;  // wheel notches light up briefly (QPC time)
  bool lmb = false;
  MouseTrail trail;
};

// Draws a layout. Used for frames by InputOverlay and for the editor's preview.
class OverlayPainter {
 public:
  explicit OverlayPainter(const OverlayConfig& cfg);  // drops keys it doesn't know
  const OverlayConfig& config() const { return cfg_; }
  float width() const { return width_; }  // layout size, key widths
  float height() const { return height_; }
  float pad_size() const { return pad_size_; }
  // Key width and the layout's top-left in a video frame of `size`.
  void Place(D2D1_SIZE_F size, float& u, D2D1_POINT_2F& origin) const;
  D2D1_RECT_F KeyRect(size_t i, D2D1_POINT_2F origin, float u) const;  // i indexes config().keys
  void Draw(ID2D1DeviceContext* dc, D2D1_POINT_2F origin, float u, OverlayInput& in, int64_t now);

 private:
  void Fill(ID2D1DeviceContext* dc, const D2D1_RECT_F& r, float u, bool on);
  void DrawPad(ID2D1DeviceContext* dc, D2D1_RECT_F r, float u, OverlayInput& in, int64_t now);
  D2D1_COLOR_F Faded(D2D1_COLOR_F c, float alpha = 1) const;

  OverlayConfig cfg_;
  std::vector<std::wstring> labels_;
  float width_ = 0, height_ = 0, pad_size_ = 1;
  D2D1_COLOR_F idle_, pressed_, text_, pressed_text_, border_, trail_, click_;

  winrt::com_ptr<ID2D1DeviceContext> res_dc_;  // resources below belong to this context at this key size; held,
                                               // so a new context can't reuse its address and look like it
  float res_u_ = 0;
  winrt::com_ptr<ID2D1SolidColorBrush> brush_;
  winrt::com_ptr<ID2D1StrokeStyle> round_;
  winrt::com_ptr<IDWriteTextFormat> text_format_;
};

// Keys and mouse movement of the game being recorded, drawn onto frames. Reads Raw Input only while
// the game has focus, so nothing typed into other apps shows up.
class InputOverlay {
 public:
  InputOverlay(const OverlayConfig& cfg, DWORD game_pid);
  ~InputOverlay();
  const OverlayConfig& config() const { return cfg_; }
  void Draw(ID2D1DeviceContext* dc, D2D1_SIZE_F frame);  // video thread only

 private:
  void OnInput(const RAWINPUT& ri);

  const OverlayConfig cfg_;
  const DWORD pid_;
  OverlayPainter painter_;
  std::mutex m_;
  OverlayInput in_;
  int listen_id_;
};

// The tray's "Edit overlay" window. `save` gets the edited settings.
void ShowOverlayEditor(const OverlayConfig& cfg, std::function<void(const OverlayConfig&)> save);
