#pragma once
#include "common.h"

#include <filesystem>
#include <string>

// A small red card that slides into the top-right corner of the game's screen ("Clip saved"). It replaces the
// Windows notification, which Windows hides while a game is fullscreen; it never takes focus or clicks and is
// left out of every capture, so it's in no clip or stream. Main thread only.
// `waiting` keeps it up until the next ShowToast (say, once the clipboard copy is ready), at most 10 seconds;
// a ShowToast while it's up changes the text in place.
void ShowToast(HWND over, const std::wstring& title, const std::wstring& detail, bool ok = true, bool waiting = false);
void HideToast();
// The card as a 32-bit BMP, for checking its look without showing it (clip_bench toast).
bool ToastSnapshot(const std::filesystem::path& bmp, const std::wstring& title, const std::wstring& detail, bool ok,
                   float dpi);
