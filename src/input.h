#pragma once
#include "common.h"

#include <functional>

// Raw Input keyboard and mouse events, even while a game has focus (no hooks, nothing in the game).
// Windows sends a process's Raw Input to one window, so every reader shares this one; it runs only
// while someone listens. Listeners are called on the input thread and must not call Listen/Unlisten.
namespace raw_input {
int Listen(std::function<void(const RAWINPUT&)> f);
void Unlisten(int id);  // no calls to that listener after this returns
}  // namespace raw_input

// "RCtrl", "LShift", "F13", "A"… for a raw keyboard event, or "" for keys Windows doesn't identify.
std::string RawKeyName(const RAWKEYBOARD& k);

// Calls `fire` (on the input thread) when `key` is pressed twice quickly with nothing in between.
class DoubleTap {
 public:
  DoubleTap(std::string key, std::function<void()> fire);
  ~DoubleTap();

 private:
  void OnInput(const RAWINPUT& ri);

  const std::string key_;
  std::function<void()> fire_;
  std::string held_;  // last key down, to ignore auto-repeat
  int64_t last_tap_ = 0;
  int id_;
};
