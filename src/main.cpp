#include "session.h"

#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>
#include <toml++/toml.hpp>

namespace fs = std::filesystem;

namespace {
enum : UINT { WM_TRAY = WM_APP + 1, WM_GAME_EXIT, WM_WINDOW_CLOSED, WM_SAVED };
enum : UINT {
  ID_SAVE = 1, ID_PAUSE, ID_OPEN_FOLDER, ID_OPEN_CONFIG, ID_QUIT, ID_NEVER, ID_ALWAYS, ID_GAME_AUDIO, ID_MIC,
  ID_LENGTH = 100,  // + index into kLengths
  ID_APP = 200,     // + index into g.menu_apps
};
enum : UINT_PTR { kReloadTimer = 1, kSyncTimer };
constexpr int kHotkey = 1;
constexpr int kLengths[] = {15, 30, 60, 120, 300};

// Apps that can fill the screen but aren't games. "Always record" in the tray overrides this.
const std::set<std::string> kNotGames = {
    "explorer.exe", "applicationframehost.exe", "searchhost.exe", "startmenuexperiencehost.exe",
    "shellexperiencehost.exe", "lockapp.exe", "textinputhost.exe", "chrome.exe", "msedge.exe", "firefox.exe",
    "opera.exe", "opera_gx.exe", "brave.exe", "vivaldi.exe", "vlc.exe", "mpc-hc64.exe", "mpc-be64.exe",
    "potplayermini64.exe", "wmplayer.exe", "microsoft.media.player.exe", "steam.exe", "steamwebhelper.exe",
    "obs64.exe", "discord.exe", "spotify.exe", "teams.exe", "ms-teams.exe", "zoom.exe", "powerpnt.exe",
    "code.exe", "windowsterminal.exe", "screenclippinghost.exe", "snippingtool.exe", "gamebar.exe",
    "powertoys.exe", "magnify.exe"};

struct {
  HWND wnd = nullptr;
  NOTIFYICONDATAW nid{};
  Config cfg;
  fs::path cfg_path;
  std::string hotkey;  // currently registered
  std::unique_ptr<Session> session;
  HANDLE proc = nullptr, exit_wait = nullptr;
  bool paused = false;
  HWINEVENTHOOK loc_hook = nullptr;
  DWORD loc_pid = 0;
  HWND last_app_wnd = nullptr;  // last foreground non-game, for "Always record"
  std::string last_app_exe;
  std::set<std::string> warned;  // exes we already showed a capture error for
  std::vector<std::string> menu_apps;
  ULONGLONG last_save = 0;
} g;

std::mutex g_log_m;
std::ofstream g_log;

bool Contains(const std::vector<std::string>& v, const std::string& s) { return std::ranges::find(v, s) != v.end(); }

fs::path KnownFolder(REFKNOWNFOLDERID id) {
  PWSTR p = nullptr;
  fs::path r;
  if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) r = p;
  CoTaskMemFree(p);
  return r;
}

std::string ExeName(DWORD pid) {
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!h) return {};
  wchar_t buf[MAX_PATH * 2];
  DWORD n = DWORD(std::size(buf));
  std::string r;
  if (QueryFullProcessImageNameW(h, 0, buf, &n)) r = Lower(Utf8(fs::path(buf).filename().wstring()));
  CloseHandle(h);
  return r;
}

// ---- tray ----

void AddTrayIcon() {
  g.nid.cbSize = sizeof g.nid;
  g.nid.hWnd = g.wnd;
  g.nid.uID = 1;
  g.nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  g.nid.uCallbackMessage = WM_TRAY;
  g.nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  wcscpy_s(g.nid.szTip, L"Clip");
  Shell_NotifyIconW(NIM_ADD, &g.nid);
}

void UpdateTip() {
  std::wstring tip = g.paused    ? L"Clip: paused"
                     : g.session ? L"Clip: recording " + Wide(g.session->exe)
                                 : L"Clip: waiting for a game";
  wcsncpy_s(g.nid.szTip, tip.c_str(), _TRUNCATE);
  g.nid.uFlags = NIF_TIP;
  Shell_NotifyIconW(NIM_MODIFY, &g.nid);
}

void Balloon(const std::wstring& title, const std::wstring& text) {
  Log("{}: {}", Utf8(title), Utf8(text));
  g.nid.uFlags = NIF_INFO;
  g.nid.dwInfoFlags = NIIF_WARNING;
  wcsncpy_s(g.nid.szInfoTitle, title.c_str(), _TRUNCATE);
  wcsncpy_s(g.nid.szInfo, text.c_str(), _TRUNCATE);
  Shell_NotifyIconW(NIM_MODIFY, &g.nid);
}

// ---- config ----

fs::path ClipsFolder() {
  return g.cfg.clips_folder.empty() ? KnownFolder(FOLDERID_Videos) / L"Clip" : fs::path(Wide(g.cfg.clips_folder));
}

Config ParseConfig(const std::string& text) {
  auto t = toml::parse(text);
  auto list = [&](const char* key) {
    std::vector<std::string> v;
    if (auto a = t[key].as_array())
      for (auto& e : *a)
        if (auto s = e.value<std::string>()) v.push_back(Lower(*s));
    return v;
  };
  Config c;
  c.clip_seconds = std::clamp(t["clip_seconds"].value_or(c.clip_seconds), 15, 300);
  c.hotkey = t["hotkey"].value_or(c.hotkey);
  c.fps = std::clamp(t["fps"].value_or(c.fps), 10, 240);
  c.max_height = std::clamp(t["max_height"].value_or(c.max_height), 360, 2160);
  c.bitrate_mbps = std::clamp(t["bitrate_mbps"].value_or(c.bitrate_mbps), 1, 150);
  c.clips_folder = t["clips_folder"].value_or(c.clips_folder);
  c.min_free_gb = std::clamp(t["min_free_gb"].value_or(c.min_free_gb), 0, 1000);
  c.record_game_audio = t["record_game_audio"].value_or(c.record_game_audio);
  c.record_mic = t["record_mic"].value_or(c.record_mic);
  c.apps = list("apps");
  c.always_record = list("always_record");
  c.never_record = list("never_record");
  c.start_with_windows = t["start_with_windows"].value_or(c.start_with_windows);
  return c;
}

void SaveConfig() {
  auto arr = [](const std::vector<std::string>& v) {
    toml::array a;
    for (auto& s : v) a.push_back(s);
    return a;
  };
  const Config& c = g.cfg;
  toml::table t{
      {"clip_seconds", c.clip_seconds},   {"hotkey", c.hotkey},
      {"fps", c.fps},                     {"max_height", c.max_height},
      {"bitrate_mbps", c.bitrate_mbps},   {"clips_folder", c.clips_folder},
      {"min_free_gb", c.min_free_gb},     {"record_game_audio", c.record_game_audio},
      {"record_mic", c.record_mic},       {"apps", arr(c.apps)},
      {"always_record", arr(c.always_record)}, {"never_record", arr(c.never_record)},
      {"start_with_windows", c.start_with_windows},
  };
  std::ofstream f(g.cfg_path, std::ios::trunc);
  f << "# Clip settings. Edits apply while Clip runs; fps, max_height and bitrate_mbps from the next game.\n"
       "# hotkey: e.g. \"Alt+F9\", \"Ctrl+Shift+S\". clips_folder: empty = Videos\\Clip.\n"
       "# apps / always_record / never_record: exe names, e.g. [\"spotify.exe\"].\n\n"
    << t << "\n";
}

bool ParseHotkey(const std::string& s, UINT& mods, UINT& vk) {
  mods = MOD_NOREPEAT;
  vk = 0;
  std::stringstream ss(Lower(s));
  for (std::string k; std::getline(ss, k, '+');) {
    std::erase(k, ' ');
    if (k == "ctrl" || k == "control") mods |= MOD_CONTROL;
    else if (k == "alt") mods |= MOD_ALT;
    else if (k == "shift") mods |= MOD_SHIFT;
    else if (k == "win") mods |= MOD_WIN;
    else if (k.size() == 1 && std::isalnum(uint8_t(k[0]))) vk = UINT(std::toupper(uint8_t(k[0])));
    else if (k.size() > 1 && k[0] == 'f' && std::all_of(k.begin() + 1, k.end(), ::isdigit) && std::stoi(k.substr(1)) >= 1 &&
             std::stoi(k.substr(1)) <= 24)
      vk = VK_F1 + std::stoi(k.substr(1)) - 1;
    else if (k == "insert") vk = VK_INSERT;
    else if (k == "home") vk = VK_HOME;
    else if (k == "end") vk = VK_END;
    else if (k == "pageup") vk = VK_PRIOR;
    else if (k == "pagedown") vk = VK_NEXT;
    else if (k == "pause") vk = VK_PAUSE;
    else if (k == "scrolllock") vk = VK_SCROLL;
    else return false;
  }
  return vk != 0;
}

void SetRunAtLogin(bool on) {
  HKEY k;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &k))
    return;
  if (on) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring v = L"\"" + std::wstring(exe) + L"\"";
    RegSetValueExW(k, L"Clip", 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()), DWORD((v.size() + 1) * 2));
  } else {
    RegDeleteValueW(k, L"Clip");
  }
  RegCloseKey(k);
}

void StopSession();

void ApplyConfig() {
  if (g.cfg.hotkey != g.hotkey) {
    UnregisterHotKey(g.wnd, kHotkey);
    g.hotkey = g.cfg.hotkey;
    UINT mods, vk;
    if (!ParseHotkey(g.cfg.hotkey, mods, vk))
      Balloon(L"Unknown hotkey", Wide(g.cfg.hotkey) + L" isn't a hotkey Clip understands. Fix it in settings.");
    else if (!RegisterHotKey(g.wnd, kHotkey, mods, vk))
      Balloon(L"Hotkey taken", Wide(g.cfg.hotkey) + L" is used by another app. Pick another in settings.");
  }
  SetRunAtLogin(g.cfg.start_with_windows);
  if (!g.session) return;
  if (Contains(g.cfg.never_record, g.session->exe)) return StopSession();
  g.session->SetClipSeconds(g.cfg.clip_seconds);
  g.session->SyncAudio(g.cfg);
}

void LoadConfig() {
  std::ifstream f(g.cfg_path);
  if (!f) {  // first run: write the defaults so there's something to edit
    SaveConfig();
    return ApplyConfig();
  }
  std::stringstream ss;
  ss << f.rdbuf();
  try {
    g.cfg = ParseConfig(ss.str());
  } catch (const toml::parse_error& e) {
    return Balloon(L"Settings not loaded", L"config.toml: " + Wide(std::string(e.description())));
  }
  ApplyConfig();
}

void ChangeConfig(auto&& edit) {
  edit(g.cfg);
  SaveConfig();
  ApplyConfig();
}

// ---- recording ----

void StopSession() {
  if (g.exit_wait) UnregisterWaitEx(g.exit_wait, INVALID_HANDLE_VALUE);
  if (g.proc) CloseHandle(g.proc);
  g.exit_wait = g.proc = nullptr;
  g.session.reset();
  KillTimer(g.wnd, kSyncTimer);
  UpdateTip();
}

void StartSession(HWND w, DWORD pid, const std::string& exe) {
  StopSession();
  std::string err;
  try {
    g.session = std::make_unique<Session>(w, pid, exe, g.cfg, [] { PostMessageW(g.wnd, WM_WINDOW_CLOSED, 0, 0); });
  } catch (const winrt::hresult_error& e) {
    err = winrt::to_string(e.message());
  } catch (const std::exception& e) {
    err = e.what();
  }
  if (!err.empty()) {
    Log("can't record {}: {}", exe, err);
    if (g.warned.insert(exe).second)
      Balloon(L"Can't record " + Wide(exe),
              Wide(err) + L"\nIf the game runs as administrator, run Clip as administrator too.");
    return UpdateTip();
  }
  g.proc = OpenProcess(SYNCHRONIZE, FALSE, pid);
  if (g.proc)
    RegisterWaitForSingleObject(
        &g.exit_wait, g.proc,
        [](void* p, BOOLEAN) { PostMessageW(g.wnd, WM_GAME_EXIT, WPARAM(uintptr_t(p)), 0); },
        reinterpret_cast<void*>(uintptr_t(pid)), INFINITE, WT_EXECUTEONLYONCE);
  SetTimer(g.wnd, kSyncTimer, 5000, nullptr);
  UpdateTip();
}

bool FillsMonitor(HWND w) {
  RECT r;
  MONITORINFO mi{sizeof mi};
  if (!GetWindowRect(w, &r) || !GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &mi)) return false;
  return r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top && r.right >= mi.rcMonitor.right &&
         r.bottom >= mi.rcMonitor.bottom;
}

void CALLBACK OnWinEvent(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);

// Watch the foreground app's window moves only while it might still turn into a fullscreen game.
void WatchLocation(DWORD pid) {
  if (pid == g.loc_pid) return;
  if (g.loc_hook) UnhookWinEvent(g.loc_hook);
  g.loc_hook = pid ? SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, OnWinEvent,
                                     pid, 0, WINEVENT_OUTOFCONTEXT)
                   : nullptr;
  g.loc_pid = pid;
}

void CheckForeground() {
  HWND w = GetForegroundWindow();
  DWORD pid = 0;
  GetWindowThreadProcessId(w, &pid);
  if (!w || !pid || pid == GetCurrentProcessId() || !IsWindowVisible(w)) return;

  if (g.session && g.session->pid == pid) {  // the game we're recording; follow it if it made a new window
    WatchLocation(0);
    if (w != g.session->hwnd && (!IsWindow(g.session->hwnd) || FillsMonitor(w))) g.session->SetWindow(w);
    return;
  }
  const std::string exe = ExeName(pid);
  if (exe.empty() || Contains(g.cfg.never_record, exe)) return WatchLocation(0);
  const bool always = Contains(g.cfg.always_record, exe);
  if (!always && (kNotGames.contains(exe) || exe.ends_with(".scr"))) return WatchLocation(0);
  if (!always && !FillsMonitor(w)) {
    g.last_app_wnd = w;
    g.last_app_exe = exe;
    return WatchLocation(pid);
  }
  WatchLocation(0);
  if (!g.paused) StartSession(w, pid, exe);
}

void CALLBACK OnWinEvent(HWINEVENTHOOK, DWORD ev, HWND w, LONG obj, LONG, DWORD, DWORD) {
  if (ev == EVENT_OBJECT_LOCATIONCHANGE && (obj != OBJID_WINDOW || w != GetForegroundWindow())) return;
  CheckForeground();
}

void SaveClip() {
  if (!g.session) return (void)PlaySoundW(L"SystemExclamation", nullptr, SND_ALIAS | SND_ASYNC);
  const ULONGLONG now = GetTickCount64();
  if (now - g.last_save < 1000) return;  // double press
  g.last_save = now;

  const std::wstring game = Wide(Stem(g.session->exe));
  const fs::path folder = ClipsFolder() / game;
  std::error_code ec;
  fs::create_directories(folder, ec);
  ULARGE_INTEGER free;
  if (GetDiskFreeSpaceExW(folder.c_str(), &free, nullptr, nullptr) &&
      free.QuadPart < uint64_t(g.cfg.min_free_gb) << 30) {
    PlaySoundW(L"SystemHand", nullptr, SND_ALIAS | SND_ASYNC);
    return Balloon(L"Clip not saved", std::format(L"Less than {} GB free on the clips drive.", g.cfg.min_free_gb));
  }
  SYSTEMTIME t;
  GetLocalTime(&t);
  auto name = std::format(L"{} {:04}-{:02}-{:02} {:02}-{:02}-{:02}.mp4", game, t.wYear, t.wMonth, t.wDay, t.wHour,
                          t.wMinute, t.wSecond);
  g.session->Save(g.cfg.clip_seconds, folder / name, [](std::string err) {
    PostMessageW(g.wnd, WM_SAVED, 0, LPARAM(new std::string(std::move(err))));
  });
}

// ---- menu ----

std::vector<std::string> AudioApps() {
  std::vector<std::string> r = g.cfg.apps;
  try {  // plus every app with an audio session right now
    auto devices = winrt::create_instance<IMMDeviceEnumerator>(__uuidof(MMDeviceEnumerator));
    com_ptr<IMMDevice> dev;
    winrt::check_hresult(devices->GetDefaultAudioEndpoint(eRender, eConsole, dev.put()));
    com_ptr<IAudioSessionManager2> mgr;
    winrt::check_hresult(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, mgr.put_void()));
    com_ptr<IAudioSessionEnumerator> list;
    winrt::check_hresult(mgr->GetSessionEnumerator(list.put()));
    int n = 0;
    list->GetCount(&n);
    for (int i = 0; i < n; i++) {
      com_ptr<IAudioSessionControl> c;
      DWORD pid = 0;
      if (FAILED(list->GetSession(i, c.put()))) continue;
      auto c2 = c.try_as<IAudioSessionControl2>();
      if (c2 && c2->IsSystemSoundsSession() == S_FALSE && SUCCEEDED(c2->GetProcessId(&pid)) && pid)
        if (auto exe = ExeName(pid); !exe.empty()) r.push_back(exe);
    }
  } catch (...) {
  }
  const std::string self = ExeName(GetCurrentProcessId());
  std::erase_if(r, [&](auto& e) { return e == self || (g.session && e == g.session->exe); });
  std::ranges::sort(r);
  r.erase(std::unique(r.begin(), r.end()), r.end());
  return r;
}

void OnCommand(UINT id) {
  if (id >= ID_APP && id - ID_APP < g.menu_apps.size()) {
    const std::string app = g.menu_apps[id - ID_APP];
    return ChangeConfig([&](Config& c) {
      if (Contains(c.apps, app)) std::erase(c.apps, app);
      else c.apps.push_back(app);
    });
  }
  if (id >= ID_LENGTH && id - ID_LENGTH < std::size(kLengths))
    return ChangeConfig([&](Config& c) { c.clip_seconds = kLengths[id - ID_LENGTH]; });
  switch (id) {
    case ID_SAVE: return SaveClip();
    case ID_GAME_AUDIO: return ChangeConfig([](Config& c) { c.record_game_audio = !c.record_game_audio; });
    case ID_MIC: return ChangeConfig([](Config& c) { c.record_mic = !c.record_mic; });
    case ID_NEVER:
      if (g.session) {
        const std::string exe = g.session->exe;
        ChangeConfig([&](Config& c) {
          std::erase(c.always_record, exe);
          c.never_record.push_back(exe);
        });
      }
      return;
    case ID_ALWAYS: {
      const std::string exe = g.last_app_exe;
      ChangeConfig([&](Config& c) {
        std::erase(c.never_record, exe);
        c.always_record.push_back(exe);
      });
      DWORD pid = 0;
      GetWindowThreadProcessId(g.last_app_wnd, &pid);
      if (IsWindow(g.last_app_wnd) && !g.paused) StartSession(g.last_app_wnd, pid, exe);
      return;
    }
    case ID_PAUSE:
      g.paused = !g.paused;
      if (g.paused) StopSession();
      else CheckForeground();
      return UpdateTip();
    case ID_OPEN_FOLDER: {
      std::error_code ec;
      fs::create_directories(ClipsFolder(), ec);
      ShellExecuteW(nullptr, L"open", ClipsFolder().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
      return;
    }
    case ID_OPEN_CONFIG:
      ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + g.cfg_path.wstring() + L"\"").c_str(), nullptr,
                    SW_SHOWNORMAL);
      return;
    case ID_QUIT: DestroyWindow(g.wnd); return;
  }
}

void ShowMenu() {
  const Config& c = g.cfg;
  auto check = [](bool on) { return MF_STRING | (on ? MF_CHECKED : 0u); };
  HMENU m = CreatePopupMenu();
  std::wstring status = g.paused ? L"Paused" : g.session ? L"Recording " + Wide(g.session->exe) : L"Waiting for a game";
  AppendMenuW(m, MF_STRING | MF_GRAYED, 0, status.c_str());
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING | (g.session ? 0 : MF_GRAYED), ID_SAVE, (L"Save clip\t" + Wide(c.hotkey)).c_str());

  HMENU len = CreatePopupMenu();
  for (UINT i = 0; i < std::size(kLengths); i++)
    AppendMenuW(len, check(kLengths[i] == c.clip_seconds), ID_LENGTH + i, std::format(L"{} s", kLengths[i]).c_str());
  if (std::ranges::find(kLengths, c.clip_seconds) == std::end(kLengths))
    AppendMenuW(len, MF_STRING | MF_CHECKED | MF_GRAYED, 0, std::format(L"{} s (from settings)", c.clip_seconds).c_str());
  AppendMenuW(m, MF_POPUP, UINT_PTR(len), std::format(L"Clip length: {} s", c.clip_seconds).c_str());

  HMENU audio = CreatePopupMenu();
  AppendMenuW(audio, check(c.record_game_audio), ID_GAME_AUDIO, L"Game");
  AppendMenuW(audio, check(c.record_mic), ID_MIC, L"Microphone");
  g.menu_apps = AudioApps();
  if (!g.menu_apps.empty()) AppendMenuW(audio, MF_SEPARATOR, 0, nullptr);
  for (UINT i = 0; i < g.menu_apps.size(); i++)
    AppendMenuW(audio, check(Contains(c.apps, g.menu_apps[i])), ID_APP + i, Wide(Stem(g.menu_apps[i])).c_str());
  AppendMenuW(m, MF_POPUP, UINT_PTR(audio), L"Audio");

  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  if (g.session)
    AppendMenuW(m, MF_STRING, ID_NEVER, (L"Never record " + Wide(g.session->exe)).c_str());
  else if (!g.last_app_exe.empty() && IsWindow(g.last_app_wnd))
    AppendMenuW(m, MF_STRING, ID_ALWAYS, (L"Always record " + Wide(g.last_app_exe)).c_str());
  AppendMenuW(m, check(g.paused), ID_PAUSE, L"Pause recording");
  AppendMenuW(m, MF_STRING, ID_OPEN_FOLDER, L"Open clips folder");
  AppendMenuW(m, MF_STRING, ID_OPEN_CONFIG, L"Edit settings");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, ID_QUIT, L"Quit");

  POINT p;
  GetCursorPos(&p);
  SetForegroundWindow(g.wnd);  // so the menu closes when clicking elsewhere
  UINT id = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, p.x, p.y, 0, g.wnd, nullptr);
  DestroyMenu(m);
  OnCommand(id);
}

// ---- window ----

const UINT WM_TASKBAR_CREATED = RegisterWindowMessageW(L"TaskbarCreated");

LRESULT CALLBACK WndProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_TASKBAR_CREATED) {  // Explorer restarted
    AddTrayIcon();
    UpdateTip();
    return 0;
  }
  switch (msg) {
    case WM_TRAY:
      if (lp == WM_RBUTTONUP || lp == WM_LBUTTONUP) ShowMenu();
      return 0;
    case WM_HOTKEY: SaveClip(); return 0;
    case WM_TIMER:
      if (wp == kReloadTimer) {
        KillTimer(w, kReloadTimer);
        LoadConfig();
      } else if (wp == kSyncTimer && g.session) {
        if (g.session->failed()) {  // encoder or capture broke: start over on the same window
          Log("restarting capture");
          HWND hwnd = g.session->hwnd;
          DWORD pid = g.session->pid;
          std::string exe = g.session->exe;
          StopSession();
          if (IsWindow(hwnd)) StartSession(hwnd, pid, exe);
        } else {
          g.session->SyncAudio(g.cfg);  // apps that started or quit
        }
      }
      return 0;
    case WM_GAME_EXIT:
      if (g.session && g.session->pid == DWORD(wp)) {
        StopSession();
        CheckForeground();
      }
      return 0;
    case WM_WINDOW_CLOSED:
      if (g.session && !IsWindow(g.session->hwnd)) Log("game window closed, waiting for its next one");
      return 0;
    case WM_SAVED: {
      std::unique_ptr<std::string> err(reinterpret_cast<std::string*>(lp));
      if (err->empty()) {
        PlaySoundW(L"SystemAsterisk", nullptr, SND_ALIAS | SND_ASYNC);
      } else {
        PlaySoundW(L"SystemHand", nullptr, SND_ALIAS | SND_ASYNC);
        Balloon(L"Clip not saved", Wide(*err));
      }
      return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
  }
  return DefWindowProcW(w, msg, wp, lp);
}

bool IsWindows11() {
  using Fn = LONG(WINAPI*)(OSVERSIONINFOW*);
  auto fn = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
  OSVERSIONINFOW v{sizeof v};
  return fn && fn(&v) == 0 && v.dwBuildNumber >= 22000;
}

void FfmpegLog(void* avcl, int level, const char* fmt, va_list vl) {
  if (level > AV_LOG_WARNING) return;
  char line[1024];
  int prefix = 1;
  av_log_format_line(avcl, level, fmt, vl, line, sizeof line, &prefix);
  std::string s = line;
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
  Log("ffmpeg: {}", s);
}
}  // namespace

void LogLine(const std::string& s) {
  SYSTEMTIME t;
  GetLocalTime(&t);
  auto line = std::format("{:02}:{:02}:{:02}.{:03} {}\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, s);
  std::lock_guard l(g_log_m);
  if (g_log) g_log << line << std::flush;
  OutputDebugStringA(line.c_str());
}

std::string Utf8(std::wstring_view w) {
  std::string s(WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), int(s.size()), nullptr, nullptr);
  return s;
}

std::wstring Wide(std::string_view s) {
  std::wstring w(MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), int(w.size()));
  return w;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);  // real pixels for the fullscreen test
  if (!IsWindows11()) {
    MessageBoxW(nullptr, L"Clip needs Windows 11.", L"Clip", MB_ICONERROR);
    return 1;
  }
  CreateMutexW(nullptr, TRUE, L"Local\\Clip.SingleInstance");
  if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
  winrt::init_apartment(winrt::apartment_type::single_threaded);

  const fs::path cfg_dir = KnownFolder(FOLDERID_RoamingAppData) / L"Clip";
  const fs::path log_dir = KnownFolder(FOLDERID_LocalAppData) / L"Clip";  // apart from config, so logging isn't an edit
  std::error_code ec;
  fs::create_directories(cfg_dir, ec);
  fs::create_directories(log_dir, ec);
  g.cfg_path = cfg_dir / L"config.toml";
  const fs::path log_path = log_dir / L"clip.log";
  const bool big = fs::file_size(log_path, ec) > (1 << 20);
  g_log.open(log_path, big ? std::ios::trunc : std::ios::app);
  Log("Clip started");
  av_log_set_callback(FfmpegLog);

  WNDCLASSW wc{};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.lpszClassName = L"ClipTray";
  RegisterClassW(&wc);
  // A hidden top-level window rather than message-only, so tray menus and TaskbarCreated work.
  g.wnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"Clip", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst,
                          nullptr);
  AddTrayIcon();
  LoadConfig();
  UpdateTip();

  HWINEVENTHOOK fg_hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, OnWinEvent, 0, 0,
                                          WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
  HANDLE watch =
      FindFirstChangeNotificationW(cfg_dir.c_str(), FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME);
  const DWORD n_handles = watch != INVALID_HANDLE_VALUE ? 1 : 0;
  CheckForeground();

  for (MSG msg;;) {
    const DWORD r = MsgWaitForMultipleObjects(n_handles, &watch, FALSE, INFINITE, QS_ALLINPUT);
    if (n_handles && r == WAIT_OBJECT_0) {
      FindNextChangeNotification(watch);
      SetTimer(g.wnd, kReloadTimer, 300, nullptr);  // editors write in bursts
      continue;
    }
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) {
        StopSession();
        UnhookWinEvent(fg_hook);
        WatchLocation(0);
        Shell_NotifyIconW(NIM_DELETE, &g.nid);
        Log("Clip quit");
        return 0;
      }
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
}
