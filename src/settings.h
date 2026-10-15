#pragma once
#include "common.h"

#include <filesystem>
#include <functional>

// The settings window: settings.html (built into the exe) in WebView2, talking to the app in JSON
// messages. It only exists while open, so it costs nothing while games record.
void ShowSettings(std::function<void(const std::string& json)> on_message);
void SettingsPost(const std::string& json);  // dropped while the window is closed
HWND SettingsWindow();                       // nullptr while closed
// Lets the page load files from `folder` as https://<host>/... (the clip gallery).
void SettingsServe(const std::wstring& host, const std::filesystem::path& folder);
