#pragma once
#include "common.h"

#include <filesystem>
#include <functional>

// The app window: ui/index.html (built into the exe) in WebView2, talking to the app in JSON messages.
// Closed, it stays hidden and ready for 10 minutes; a game starting frees it at once. Closing also sends
// on_message {"type":"hidden"}.
void ShowSettings(std::function<void(const std::string& json)> on_message);
void SettingsPost(const std::string& json);  // dropped while the window is closed
HWND SettingsWindow();                       // nullptr while closed
// Lets the page load files from `folder` as https://<host>/... (the clip gallery).
void SettingsServe(const std::wstring& host, const std::filesystem::path& folder);
bool SettingsVisible();
// Closing only hides the window (reopening is instant); this frees a hidden one now, e.g. when a game starts.
void SettingsForgetHidden();
