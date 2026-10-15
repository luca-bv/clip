#include <unknwn.h>

#include "games.h"

#include <shlobj.h>
#include <urlmon.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

#include "common.h"

namespace {
std::mutex m;
std::map<std::string, GameInfo> games;  // lowercase exe without ".exe"

std::string Key(std::string exe) {
  exe = Lower(exe.substr(exe.find_last_of("/\\") + 1));
  return exe.ends_with(".exe") ? exe.substr(0, exe.size() - 4) : exe;
}

void Load() {
  namespace fs = std::filesystem;
  namespace json = winrt::Windows::Data::Json;
  PWSTR local = nullptr;
  SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local);
  const fs::path file = fs::path(local ? local : L".") / L"Clip" / L"discord-games.json";
  CoTaskMemFree(local);
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  const bool fresh =
      fs::exists(file, ec) && fs::file_time_type::clock::now() - fs::last_write_time(file, ec) < std::chrono::hours(24 * 7);
  if (!fresh && FAILED(URLDownloadToFileW(nullptr, L"https://discord.com/api/v9/applications/detectable", file.c_str(), 0,
                                          nullptr)))
    Log("game list: download failed");
  std::ifstream f(file, std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(f)), {});
  json::JsonArray a{nullptr};
  if (!json::JsonArray::TryParse(Wide(text), a)) return Log("game list: unreadable");
  std::map<std::string, GameInfo> found;
  for (auto v : a) {
    const auto g = v.GetObject();
    if (!g.HasKey(L"executables")) continue;
    std::string name = winrt::to_string(g.GetNamedString(L"name", L""));
    if (g.HasKey(L"aliases"))  // "ROBLOX" is "Roblox" in its aliases
      for (auto al : g.GetNamedArray(L"aliases"))
        if (al.ValueType() == json::JsonValueType::String && Lower(winrt::to_string(al.GetString())) == Lower(name))
          name = winrt::to_string(al.GetString());
    std::string icon;
    if (g.HasKey(L"icon_hash") && g.GetNamedValue(L"icon_hash").ValueType() == json::JsonValueType::String)
      icon = std::format("https://cdn.discordapp.com/app-icons/{}/{}.png?size=512",
                         winrt::to_string(g.GetNamedString(L"id")), winrt::to_string(g.GetNamedString(L"icon_hash")));
    for (auto e : g.GetNamedArray(L"executables")) {
      const auto x = e.GetObject();
      if (x.GetNamedString(L"os", L"") != L"win32" || x.GetNamedBoolean(L"is_launcher", false)) continue;
      found.try_emplace(Key(winrt::to_string(x.GetNamedString(L"name", L""))), GameInfo{name, icon});
    }
  }
  Log("game list: {} games", found.size());
  std::lock_guard l(m);
  games = std::move(found);
}
}  // namespace

void LoadGameList(std::function<void()> ready) {
  std::thread([ready = std::move(ready)] {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    winrt::init_apartment();
    try {
      Load();
    } catch (const winrt::hresult_error& e) {
      Log("game list: {}", winrt::to_string(e.message()));
    }
    ready();
  }).detach();
}

std::optional<GameInfo> FindGame(const std::string& exe) {
  std::lock_guard l(m);
  const auto it = games.find(Key(exe));
  if (it == games.end()) return std::nullopt;
  return it->second;
}
