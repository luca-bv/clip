#pragma once
#include "common.h"

#include <unknwn.h>

#include <d2d1_1.h>
#include <dwrite.h>
#include <winrt/base.h>

#include <atomic>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "trail.h"

// Keys and mouse movement of the game being recorded, drawn onto frames. Reads Raw Input from its own
// thread, and only while the game has focus, so nothing typed into other apps shows up.
class InputOverlay {
 public:
  InputOverlay(const OverlayConfig& cfg, DWORD game_pid);
  ~InputOverlay();
  const OverlayConfig& config() const { return cfg_; }
  // Draws onto `dc`'s target, a frame of `size` pixels. Video thread only.
  void Draw(ID2D1DeviceContext* dc, D2D1_SIZE_F size);

 private:
  void Loop(std::promise<void>* started);
  void OnInput(HRAWINPUT h);
  void Set(const std::string& key, bool down);
  void DrawPad(ID2D1DeviceContext* dc, D2D1_RECT_F r, float u, int64_t now);
  void Fill(ID2D1DeviceContext* dc, const D2D1_RECT_F& r, float u, bool on);

  const OverlayConfig cfg_;
  const DWORD pid_;
  std::vector<OverlayKey> keys_;  // canonical names, unknown ones dropped
  std::vector<std::wstring> labels_;
  float width_ = 0, height_ = 0;  // layout size in key widths
  float pad_size_ = 1;            // sqrt of the mouse pad's area, in key widths

  std::mutex m_;
  std::set<std::string> down_;
  std::map<std::string, int64_t> lit_until_;  // wheel notches light up briefly
  bool lmb_ = false;
  MouseTrail trail_;

  std::atomic<DWORD> thread_id_{0};
  std::thread thread_;

  // Drawing resources, rebuilt when the device context or key size changes.
  ID2D1DeviceContext* res_dc_ = nullptr;
  float res_u_ = 0;
  winrt::com_ptr<ID2D1SolidColorBrush> brush_;
  winrt::com_ptr<ID2D1StrokeStyle> round_;
  winrt::com_ptr<IDWriteTextFormat> text_;
};
