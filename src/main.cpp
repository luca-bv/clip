#include "discord.h"
#include "export.h"
#include "games.h"
#include "preview.h"
#include "session.h"
#include "settings.h"
#include "voice.h"

#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <toml++/toml.hpp>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

namespace fs = std::filesystem;
namespace json = winrt::Windows::Data::Json;

namespace {
enum : UINT {
  WM_TRAY = WM_APP + 1, WM_GAME_EXIT, WM_WINDOW_CLOSED, WM_SAVED, WM_TAP_PICKED, WM_EXPORT_PROGRESS, WM_EXPORT_DONE, WM_CLIP_INFO, WM_GAMES_READY, WM_VOICE_SAVE, WM_VOICE_FAIL, WM_PREVIEW_READY
};
// Discord application whose name Discord shows as "Playing â€¦". Empty turns the Discord activity off.
constexpr char kDiscordAppId[] = "1560105311728308347";
enum : UINT {
  ID_SAVE = 1, ID_PAUSE, ID_OPEN_FOLDER, ID_OPEN_CONFIG, ID_QUIT, ID_NEVER, ID_ALWAYS, ID_GAME_AUDIO, ID_MIC,
  ID_SHORTCUT, ID_OVERLAY, ID_OVERLAY_EDIT, ID_COPY, ID_SETTINGS,
  ID_LENGTH = 100,  // + index into kLengths
  ID_APP = 200,     // + index into g.menu_apps
  ID_PRESET = 900,  // + index into OverlayPresets()
};
enum : UINT_PTR { kReloadTimer = 1, kSyncTimer, kCaptionTimer };
struct Saved {  // WM_SAVED's lParam
  std::string err;
  fs::path file;
};
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
  HWND picker = nullptr;  // "Change shortcut" window while open
  std::wstring picker_text;
  std::unique_ptr<DoubleTap> double_tap;  // listens only while recording
  std::unique_ptr<VoiceSave> voice;       // so does this
  std::string voice_error;
  std::string double_tap_key;
  std::string hotkey_error;  // why the shortcut isn't registered, shown in settings
  bool page_picking = false;  // the settings window is capturing a new shortcut
  int tap_picker = 0;         // raw input listener while settings captures a double-tap key
  fs::path last_clip;
  bool balloon_shows_clip = false;  // clicking the current notification opens last_clip
  std::wstring session_title;       // the game's own name, for file names and Discord
  int64_t session_since = 0;        // unix time the recording started
  std::unique_ptr<DiscordPresence> discord;
  fs::path paste_file;         // clip on the clipboard, rendered when pasted
  std::wstring paste_caption;  // goes on the clipboard after the clip is pasted
  ULONGLONG paste_since = 0;
  bool caption_into_discord = false;  // the clip went into Discord: paste the caption there too
  int caption_waits = 0;
  std::thread export_thread;  // one export at a time
  bool exporting = false;
  std::atomic<bool> export_cancel{false};
} g;

// Background preview maker (PreviewLoop).
struct {
  std::mutex m;
  std::condition_variable cv;
  std::deque<std::pair<fs::path, std::string>> q;  // clip, its path as the page names it
  std::set<fs::path> queued;
  std::thread thread;
  bool stop = false;
  std::atomic<bool> recording{false};
} pv;

std::mutex g_log_m;
std::ofstream g_log;

void SettingsPush();
void SettingsSay(const std::string& type, const char* key, const std::string& value);
void OpenSettings();
std::wstring GameName(DWORD pid, const std::string& exe);

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

// A filled dot in the tray's icon size, drawn at runtime so there's no icon file to ship.
HICON DotIcon(uint8_t r, uint8_t gr, uint8_t b) {
  const int n = GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForSystem());
  BITMAPINFO bi{};
  bi.bmiHeader = {DWORD(sizeof(BITMAPINFOHEADER)), n, -n, 1, 32, BI_RGB};
  void* bits = nullptr;
  HBITMAP color = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  auto* px = static_cast<uint32_t*>(bits);
  const float c = (n - 1) / 2.f, radius = n * 0.36f;
  for (int y = 0; y < n; y++)
    for (int x = 0; x < n; x++) {
      const float a = std::clamp(radius + 0.5f - std::hypot(x - c, y - c), 0.f, 1.f);  // antialiased edge
      px[y * n + x] = uint32_t(a * 255) << 24 | uint32_t(r * a) << 16 | uint32_t(gr * a) << 8 | uint32_t(b * a);
    }
  HBITMAP mask = CreateBitmap(n, n, 1, 1, nullptr);
  ICONINFO ii{TRUE, 0, 0, mask, color};
  HICON icon = CreateIconIndirect(&ii);
  DeleteObject(mask);
  DeleteObject(color);
  return icon;
}

HICON TrayIcon(bool recording) {
  static HICON red = DotIcon(232, 48, 48), grey = DotIcon(150, 150, 150);
  return recording ? red : grey;
}

void AddTrayIcon() {
  g.nid.cbSize = sizeof g.nid;
  g.nid.hWnd = g.wnd;
  g.nid.uID = 1;
  g.nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  g.nid.uCallbackMessage = WM_TRAY;
  g.nid.hIcon = TrayIcon(false);
  wcscpy_s(g.nid.szTip, L"Clip");
  Shell_NotifyIconW(NIM_ADD, &g.nid);
}

void UpdateTip() {
  std::wstring tip = g.paused    ? L"Clip: paused"
                     : g.session ? L"Clip: recording " + g.session_title
                                 : L"Clip: waiting for a game";
  wcsncpy_s(g.nid.szTip, tip.c_str(), _TRUNCATE);
  g.nid.hIcon = TrayIcon(g.session != nullptr);
  g.nid.uFlags = NIF_TIP | NIF_ICON;
  Shell_NotifyIconW(NIM_MODIFY, &g.nid);
  SettingsPush();
}

void Balloon(const std::wstring& title, const std::wstring& text, bool warning = true) {
  Log("{}: {}", Utf8(title), Utf8(text));
  g.balloon_shows_clip = false;
  g.nid.uFlags = NIF_INFO;
  g.nid.dwInfoFlags = warning ? NIIF_WARNING : NIIF_INFO;
  wcsncpy_s(g.nid.szInfoTitle, title.c_str(), _TRUNCATE);
  wcsncpy_s(g.nid.szInfo, text.c_str(), _TRUNCATE);
  Shell_NotifyIconW(NIM_MODIFY, &g.nid);
}

// A short two-note chime as an in-memory WAV, so it plays whatever the Windows sound scheme is.
const std::vector<char>& Chime(bool ok) {
  auto make = [](std::initializer_list<float> notes) {
    constexpr int rate = 44100, per = rate * 130 / 1000;
    std::vector<int16_t> pcm;
    for (float hz : notes)
      for (int i = 0; i < per; i++) {
        const float t = float(i) / rate, env = std::min(1.f, i / 150.f) * std::exp(-t * 26);
        pcm.push_back(int16_t(std::sin(6.2831853f * hz * t) * env * 9000));
      }
    const uint32_t data = uint32_t(pcm.size() * 2);
    std::vector<char> wav(44 + data);
    auto put = [&](size_t at, auto v) { memcpy(wav.data() + at, &v, sizeof v); };
    memcpy(wav.data(), "RIFF", 4);
    put(4, uint32_t(36 + data));
    memcpy(wav.data() + 8, "WAVEfmt ", 8);
    put(16, uint32_t(16));
    put(20, uint16_t(1));  // PCM
    put(22, uint16_t(1));  // mono
    put(24, uint32_t(rate));
    put(28, uint32_t(rate * 2));
    put(32, uint16_t(2));
    put(34, uint16_t(16));
    memcpy(wav.data() + 36, "data", 4);
    put(40, data);
    memcpy(wav.data() + 44, pcm.data(), data);
    return wav;
  };
  static const auto saved = make({880.f, 1318.5f}), failed = make({392.f, 293.7f});
  return ok ? saved : failed;
}

void PlayChime(bool ok) { PlaySoundW(LPCWSTR(Chime(ok).data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT); }

// "Clip saved" notification; clicking it shows the file in Explorer.
void SavedNotice(const std::wstring& what, const std::wstring& next) {
  g.nid.uFlags = NIF_INFO;
  g.nid.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;
  wcsncpy_s(g.nid.szInfoTitle, L"Clip saved", _TRUNCATE);
  const std::wstring text = what + L"\n" + next;
  wcsncpy_s(g.nid.szInfo, text.c_str(), _TRUNCATE);
  Shell_NotifyIconW(NIM_MODIFY, &g.nid);
  g.balloon_shows_clip = true;
}

void ShowLastClip() {
  if (!g.last_clip.empty())
    ShellExecuteW(nullptr, nullptr, L"explorer.exe", (L"/select,\"" + g.last_clip.wstring() + L"\"").c_str(), nullptr,
                  SW_SHOWNORMAL);
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
  c.copy_to_clipboard = t["copy_to_clipboard"].value_or(c.copy_to_clipboard);
  c.watermark = t["watermark"].value_or(c.watermark);
  c.share_size = Lower(t["share_size"].value_or(c.share_size));
  if (c.share_size != "original") c.share_size = "discord";
  c.discord_presence = t["discord_presence"].value_or(c.discord_presence);
  c.share_caption = t["share_caption"].value_or(c.share_caption);
  c.voice_save = t["voice_save"].value_or(c.voice_save);
  c.double_tap = t["double_tap"].value_or(c.double_tap);

  auto o = t["overlay"];
  auto& ov = c.overlay;
  ov.enabled = o["enabled"].value_or(ov.enabled);
  ov.corner = Lower(o["corner"].value_or(ov.corner));
  ov.scale = std::clamp(float(o["scale"].value_or(double(ov.scale))), 0.25f, 4.f);
  ov.opacity = std::clamp(float(o["opacity"].value_or(double(ov.opacity))), 0.f, 1.f);
  ov.mouse_mode = Lower(o["mouse_mode"].value_or(ov.mouse_mode));
  ov.mouse_sensitivity = std::clamp(float(o["mouse_sensitivity"].value_or(double(ov.mouse_sensitivity))), 0.01f, 100.f);
  ov.mouse_trail_length = std::clamp(float(o["mouse_trail_length"].value_or(double(ov.mouse_trail_length))), 0.f, 100.f);
  ov.mouse_trail_ms = std::clamp(o["mouse_trail_ms"].value_or(ov.mouse_trail_ms), 1, 10000);
  ov.trail_width = std::clamp(float(o["trail_width"].value_or(double(ov.trail_width))), 0.1f, 10.f);
  ov.roundness = std::clamp(float(o["roundness"].value_or(double(ov.roundness))), 0.f, 0.5f);
  ov.idle_color = o["key_color"].value_or(ov.idle_color);
  ov.pressed_color = o["pressed_color"].value_or(ov.pressed_color);
  ov.text_color = o["text_color"].value_or(ov.text_color);
  ov.pressed_text_color = o["pressed_text_color"].value_or(ov.pressed_text_color);
  ov.border_color = o["border_color"].value_or(ov.border_color);
  ov.trail_color = o["trail_color"].value_or(ov.trail_color);
  ov.click_color = o["click_color"].value_or(ov.click_color);
  ov.font = o["font"].value_or(ov.font);
  if (auto keys = o["keys"].as_array()) {
    ov.keys.clear();
    for (auto& e : *keys) {
      toml::node_view<toml::node> k{&e};
      OverlayKey key{k["key"].value_or(std::string{}), k["label"].value_or(std::string{}),
                     float(k["x"].value_or(0.0)),       float(k["y"].value_or(0.0)),
                     float(k["w"].value_or(1.0)),       float(k["h"].value_or(1.0))};
      if (!key.key.empty() && key.w > 0 && key.h > 0) ov.keys.push_back(key);
    }
  }
  return c;
}

toml::array TomlList(const std::vector<std::string>& v) {
  toml::array a;
  for (auto& s : v) a.push_back(s);
  return a;
}

// Config as written to config.toml; ParseConfig(this) gives the same config back.
toml::table ConfigTable(const Config& c) {
  auto arr = TomlList;
  toml::table t{
      {"clip_seconds", c.clip_seconds},   {"hotkey", c.hotkey},
      {"fps", c.fps},                     {"max_height", c.max_height},
      {"bitrate_mbps", c.bitrate_mbps},   {"clips_folder", c.clips_folder},
      {"min_free_gb", c.min_free_gb},     {"record_game_audio", c.record_game_audio},
      {"record_mic", c.record_mic},       {"apps", arr(c.apps)},
      {"always_record", arr(c.always_record)}, {"never_record", arr(c.never_record)},
      {"start_with_windows", c.start_with_windows}, {"copy_to_clipboard", c.copy_to_clipboard},
      {"watermark", c.watermark},         {"double_tap", c.double_tap},
      {"share_size", c.share_size},       {"discord_presence", c.discord_presence},
      {"share_caption", c.share_caption}, {"voice_save", c.voice_save},
  };
  auto num = [](toml::table& kt, const char* name, float v) {  // 2 rather than 2.0 where it's whole
    if (v == std::floor(v)) kt.insert(name, int64_t(v));
    else kt.insert(name, double(v));
  };
  const OverlayConfig& ov = c.overlay;
  toml::array keys;
  for (auto& k : ov.keys) {
    toml::table kt{{"key", k.key}};
    if (!k.label.empty()) kt.insert("label", k.label);
    num(kt, "x", k.x);
    num(kt, "y", k.y);
    if (k.w != 1) num(kt, "w", k.w);
    if (k.h != 1) num(kt, "h", k.h);
    keys.push_back(std::move(kt));
  }
  toml::table overlay{{"enabled", ov.enabled},          {"corner", ov.corner}, {"mouse_mode", ov.mouse_mode},
                      {"mouse_trail_ms", ov.mouse_trail_ms}, {"keys", std::move(keys)},
                      {"key_color", ov.idle_color},          {"pressed_color", ov.pressed_color},
                      {"text_color", ov.text_color},         {"pressed_text_color", ov.pressed_text_color},
                      {"border_color", ov.border_color},     {"trail_color", ov.trail_color},
                      {"click_color", ov.click_color},       {"font", ov.font}};
  num(overlay, "scale", ov.scale);
  num(overlay, "opacity", ov.opacity);
  num(overlay, "mouse_sensitivity", ov.mouse_sensitivity);
  num(overlay, "mouse_trail_length", ov.mouse_trail_length);
  num(overlay, "trail_width", ov.trail_width);
  num(overlay, "roundness", ov.roundness);
  t.insert("overlay", std::move(overlay));
  return t;
}

void SaveConfig() {
  const toml::table t = ConfigTable(g.cfg);
  std::ofstream f(g.cfg_path, std::ios::trunc);
  f << "# Clip settings. Edits apply while Clip runs; fps, max_height and bitrate_mbps from the next game.\n"
       "# hotkey: e.g. \"Alt+F8\", \"Ctrl+Shift+S\". double_tap: a key tapped twice saves too, e.g. \"RCtrl\", \"\" = off.\n"
       "# clips_folder: empty = Videos\\Clip. Easier: Clip's settings window (tray icon, or Clip in the Start menu).\n"
       "# apps / always_record / never_record: exe names, e.g. [\"spotify.exe\"].\n"
       "#\n"
       "# [overlay] draws your keys and mouse movement into clips, only while the game has focus.\n"
       "#   corner: top-left, top-right, bottom-left or bottom-right. scale: 1 = default size. opacity: 0 to 1.\n"
       "#   mouse_mode: \"wrap\" (the dot wraps at the pad's edges) or \"pan\" (the dot stays centred, the pad scrolls).\n"
       "#   mouse_sensitivity, mouse_trail_length, trail_width: 1 = default. mouse_trail_ms: how long the trail fades.\n"
       "#   Colours: \"#RRGGBB\" or \"#RRGGBBAA\" (AA = opacity). roundness: corner radius in key widths, 0 to 0.5.\n"
       "#   Easier: Clip's settings window > Input overlay > Edit.\n"
       "#   keys: one entry per box. key = A-Z, 0-9, F1-F24, Space, Shift, Ctrl, Alt, Tab, Enter, Esc, Caps,\n"
       "#     Backspace, Up, Down, Left, Right, LMB, RMB, MMB, Mouse4, Mouse5, WheelUp, WheelDown,\n"
       "#     or Mouse for the movement pad. x, y, w, h: position and size in key widths. label: text shown.\n\n"
    << t << "\n";
}

const std::pair<const char*, UINT> kNamedKeys[] = {{"Insert", VK_INSERT}, {"Home", VK_HOME},       {"End", VK_END},
                                                   {"PageUp", VK_PRIOR},  {"PageDown", VK_NEXT}, {"Pause", VK_PAUSE},
                                                   {"ScrollLock", VK_SCROLL}};

// Name of a key a shortcut can use ("F9", "S", "PageUp"), or "" if it can't be one.
std::string KeyName(UINT vk) {
  if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, char(vk));
  if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
  for (auto& [name, v] : kNamedKeys)
    if (v == vk) return name;
  return {};
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
    else {
      vk = 0;
      for (UINT v = 1; v < 256 && !vk; v++)
        if (auto name = KeyName(v); !name.empty() && Lower(name) == k) vk = v;
      if (!vk) return false;
    }
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
    std::wstring v = L"\"" + std::wstring(exe) + L"\" --background";  // straight to the tray at login
    RegSetValueExW(k, L"Clip", 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()), DWORD((v.size() + 1) * 2));
  } else {
    RegDeleteValueW(k, L"Clip");
  }
  RegCloseKey(k);
}

// Start menu entry for whichever Clip.exe ran last, so a zip install is findable after Quit.
void EnsureStartMenuShortcut() {
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  const fs::path lnk = KnownFolder(FOLDERID_Programs) / L"Clip.lnk";
  try {
    auto link = winrt::create_instance<IShellLinkW>(CLSID_ShellLink);
    auto file = link.as<IPersistFile>();
    wchar_t cur[MAX_PATH] = L"";
    std::error_code ec;
    if (SUCCEEDED(file->Load(lnk.c_str(), STGM_READ)) && SUCCEEDED(link->GetPath(cur, MAX_PATH, nullptr, 0)) &&
        fs::equivalent(cur, exe, ec))
      return;
    winrt::check_hresult(link->SetPath(exe));
    winrt::check_hresult(link->SetWorkingDirectory(fs::path(exe).parent_path().c_str()));
    winrt::check_hresult(link->SetDescription(L"Records your game in the background and saves clips on a hotkey"));
    winrt::check_hresult(file->Save(lnk.c_str(), TRUE));
    Log("Start menu shortcut -> {}", Utf8(exe));
  } catch (const winrt::hresult_error& e) {
    Log("can't write Start menu shortcut: {}", winrt::to_string(e.message()));
  }
}

void StopSession();

void SyncVoice() {
  const bool want = g.session && g.cfg.voice_save;
  if (want == bool(g.voice)) return;
  g.voice.reset();
  if (!want) return;
  g.voice_error.clear();
  g.voice = std::make_unique<VoiceSave>(
      [](double ago) { PostMessageW(g.wnd, WM_VOICE_SAVE, WPARAM(ago * 1000), 0); },
      [](std::string why) { PostMessageW(g.wnd, WM_VOICE_FAIL, 0, LPARAM(new std::string(std::move(why)))); });
}

void SyncDoubleTap() {
  const std::string want = g.session ? g.cfg.double_tap : "";
  if (want == g.double_tap_key) return;
  g.double_tap.reset();
  g.double_tap_key = want;
  if (!want.empty())
    g.double_tap = std::make_unique<DoubleTap>(want, [] { PostMessageW(g.wnd, WM_HOTKEY, kHotkey, 0); });
}

void SyncDiscord() {
  if (!g.cfg.discord_presence || !kDiscordAppId[0]) return g.discord.reset();
  if (!g.discord) g.discord = std::make_unique<DiscordPresence>(kDiscordAppId);
  g.discord->Show(g.session ? Utf8(g.session_title) : "", g.session ? g.session->exe : "", g.session_since);
}

void ApplyConfig() {
  if (g.cfg.hotkey != g.hotkey && !g.picker && !g.page_picking) {
    UnregisterHotKey(g.wnd, kHotkey);
    g.hotkey = g.cfg.hotkey;
    g.hotkey_error.clear();
    UINT mods, vk;
    if (!ParseHotkey(g.cfg.hotkey, mods, vk))
      g.hotkey_error = g.cfg.hotkey + " isn't a shortcut Clip understands.";
    else if (!RegisterHotKey(g.wnd, kHotkey, mods, vk))
      g.hotkey_error = g.cfg.hotkey + " is used by another app (NVIDIA's overlay uses Alt+F9 and Alt+F10).";
    // Alt+F9 was the default before NVIDIA's overlay turned out to own it: move people still on it to today's.
    if (!g.hotkey_error.empty() && g.cfg.hotkey == "Alt+F9" && Config{}.hotkey != "Alt+F9") {
      Log("old default shortcut Alt+F9 is taken; switching to {}", Config{}.hotkey);
      g.cfg.hotkey = Config{}.hotkey;
      SaveConfig();
      return ApplyConfig();
    }
    if (!g.hotkey_error.empty()) Balloon(L"Shortcut not working", Wide(g.hotkey_error) + L" Pick another in settings.");
  }
  SetRunAtLogin(g.cfg.start_with_windows);
  SyncDoubleTap();
  SyncVoice();
  SyncDiscord();
  SettingsPush();
  if (!g.session) return;
  if (Contains(g.cfg.never_record, g.session->exe)) return StopSession();
  g.session->SetClipSeconds(g.cfg.clip_seconds);
  g.session->SyncAudio(g.cfg);
  g.session->SetOverlay(g.cfg.overlay);
}

void LoadConfig() {
  std::ifstream f(g.cfg_path);
  if (!f) {  // first run: write the defaults so there's something to edit
    SaveConfig();
    ApplyConfig();
    return Balloon(L"Clip is running", L"It records games in the background. Press " + Wide(g.cfg.hotkey) +
                                           L" in a game to save the last " + std::to_wstring(g.cfg.clip_seconds) +
                                           L" seconds. Settings are in this tray icon.",
                   false);
  }
  std::stringstream ss;
  ss << f.rdbuf();
  try {
    g.cfg = ParseConfig(ss.str());
  } catch (const toml::parse_error& e) {
    return Balloon(L"Settings not loaded", L"config.toml: " + Wide(std::string(e.description())));
  }
  if (ss.str().find("[overlay") == std::string::npos) SaveConfig();  // older file: add the overlay section to edit
  ApplyConfig();
}

void ChangeConfig(auto&& edit) {
  edit(g.cfg);
  SaveConfig();
  ApplyConfig();
}

// ---- shortcut picker ----

constexpr int kProbeHotkey = 2;
const wchar_t kPickerPrompt[] = L"Press the shortcut for saving a clip.\nEsc to cancel.";

void PickerSay(const std::wstring& text) {
  g.picker_text = text + L"\n\nTry another, or press Esc to cancel.";
  InvalidateRect(g.picker, nullptr, TRUE);
}

// Sets the shortcut if it can be one and is free. Returns "" or why not; "â€¦" means wait for more keys.
std::wstring TryShortcut(UINT vk, bool ctrl, bool alt, bool shift, bool win) {
  const std::string key = KeyName(vk);
  if (key.empty()) {
    if (vk != VK_CONTROL && vk != VK_MENU && vk != VK_SHIFT && vk != VK_LWIN && vk != VK_RWIN)
      return L"That key can't be a shortcut. Use an F-key, or a letter or number with Ctrl or Alt.";
    return L"â€¦";  // a modifier on its own: wait for the rest
  }
  if (key.size() == 1 && !ctrl && !alt && !win)
    return L"Add Ctrl or Alt to a letter or number, so typing it doesn't save a clip.";
  const std::string combo = std::string(ctrl ? "Ctrl+" : "") + (alt ? "Alt+" : "") + (shift ? "Shift+" : "") +
                            (win ? "Win+" : "") + key;
  UINT mods, v;
  ParseHotkey(combo, mods, v);
  if (!RegisterHotKey(g.wnd, kProbeHotkey, mods, v)) return Wide(combo) + L" is used by another app.";
  UnregisterHotKey(g.wnd, kProbeHotkey);
  g.picker = nullptr;  // so ApplyConfig registers the new one
  g.page_picking = false;
  ChangeConfig([&](Config& c) { c.hotkey = combo; });
  PlaySoundW(L"SystemAsterisk", nullptr, SND_ALIAS | SND_ASYNC);
  return L"";
}

void OnPickerKey(UINT vk) {
  const bool ctrl = GetKeyState(VK_CONTROL) < 0, alt = GetKeyState(VK_MENU) < 0, shift = GetKeyState(VK_SHIFT) < 0,
             win = GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0;
  if (vk == VK_ESCAPE || (alt && vk == VK_F4)) return (void)DestroyWindow(g.picker);
  HWND picker = g.picker;
  const std::wstring err = TryShortcut(vk, ctrl, alt, shift, win);
  if (err.empty()) DestroyWindow(picker);
  else if (err != L"â€¦") PickerSay(err);
}

LRESULT CALLBACK PickerProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: OnPickerKey(UINT(wp)); return 0;
    case WM_CHAR:
    case WM_SYSCHAR: return 0;  // no beeps
    case WM_SYSCOMMAND:
      if ((wp & 0xFFF0) == SC_KEYMENU) return 0;  // Alt alone shouldn't open the window menu
      break;
    case WM_PAINT: {
      const UINT dpi = GetDpiForWindow(w);
      static HFONT font = [&] {
        NONCLIENTMETRICSW m{sizeof m};
        SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof m, &m, 0, dpi);
        return CreateFontIndirectW(&m.lfMessageFont);
      }();
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(w, &ps);
      RECT r;
      GetClientRect(w, &r);
      InflateRect(&r, -MulDiv(16, dpi, 96), -MulDiv(14, dpi, 96));
      SelectObject(dc, font);
      SetBkMode(dc, TRANSPARENT);
      DrawTextW(dc, g.picker_text.c_str(), -1, &r, DT_WORDBREAK);
      EndPaint(w, &ps);
      return 0;
    }
    case WM_DESTROY:
      if (g.picker == w) {  // cancelled: put the old shortcut back
        g.picker = nullptr;
        g.hotkey.clear();
        ApplyConfig();
      }
      return 0;
  }
  return DefWindowProcW(w, msg, wp, lp);
}

void ShowShortcutPicker() {
  if (g.picker) return (void)SetForegroundWindow(g.picker);
  static const bool registered = [] {
    WNDCLASSW wc{};
    wc.lpfnWndProc = PickerProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ClipShortcut";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = HBRUSH(COLOR_WINDOW + 1);
    return RegisterClassW(&wc) != 0;
  }();
  (void)registered;
  UnregisterHotKey(g.wnd, kHotkey);  // so pressing the current shortcut reaches the window
  g.hotkey.clear();
  g.picker_text = std::wstring(kPickerPrompt) + L"\n\nNow: " + Wide(g.cfg.hotkey);
  const UINT dpi = GetDpiForSystem();
  const int w = MulDiv(380, dpi, 96), h = MulDiv(170, dpi, 96);
  RECT wa;
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
  g.picker = CreateWindowExW(WS_EX_TOPMOST, L"ClipShortcut", L"Clip shortcut", WS_POPUP | WS_CAPTION | WS_SYSMENU,
                             (wa.left + wa.right - w) / 2, (wa.top + wa.bottom - h) / 2, w, h, nullptr, nullptr,
                             GetModuleHandleW(nullptr), nullptr);
  ShowWindow(g.picker, SW_SHOW);
  SetForegroundWindow(g.picker);
}

// ---- recording ----

void StopSession() {
  if (g.exit_wait) UnregisterWaitEx(g.exit_wait, INVALID_HANDLE_VALUE);
  if (g.proc) CloseHandle(g.proc);
  g.exit_wait = g.proc = nullptr;
  g.session.reset();
  pv.recording = false;
  pv.cv.notify_one();
  SyncDoubleTap();
  SyncVoice();
  SyncDiscord();
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
  g.session_title = GameName(pid, exe);
  g.session_since = int64_t(time(nullptr));
  pv.recording = true;
  SettingsForgetHidden();  // a closed window gives its memory back to the game
  SyncDoubleTap();
  SyncVoice();
  SyncDiscord();
  UpdateTip();
}

// Fullscreen or borderless: covers its monitor and has no title bar (a maximized window
// covers the monitor too when the taskbar auto-hides).
bool FillsMonitor(HWND w) {
  if ((GetWindowLongW(w, GWL_STYLE) & WS_CAPTION) == WS_CAPTION) return false;
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

// The game's own name ("Roblox" for robloxplayerbeta.exe) from Discord's game list, else its exe's version
// info, fit for a file name.
std::wstring GameName(DWORD pid, const std::string& exe) {
  std::wstring name;
  if (auto known = FindGame(exe)) name = Wide(known->name);
  else if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
    wchar_t path[MAX_PATH * 2];
    DWORD n = DWORD(std::size(path));
    if (QueryFullProcessImageNameW(h, 0, path, &n)) {
      DWORD ignored, size = GetFileVersionInfoSizeW(path, &ignored);
      std::vector<char> info(size);
      struct Lang { WORD lang, cp; }* langs;
      UINT len;
      if (size && GetFileVersionInfoW(path, 0, size, info.data()) &&
          VerQueryValueW(info.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&langs), &len) && len) {
        wchar_t* desc;
        const auto key = std::format(L"\\StringFileInfo\\{:04x}{:04x}\\FileDescription", langs->lang, langs->cp);
        if (VerQueryValueW(info.data(), key.c_str(), reinterpret_cast<void**>(&desc), &len) && len > 1) name = desc;
      }
    }
    CloseHandle(h);
  }
  std::erase_if(name, [](wchar_t c) { return c < 32 || std::wcschr(L"<>:\"/\\|?*", c); });
  while (!name.empty() && (name.back() == L' ' || name.back() == L'.')) name.pop_back();
  if (name.empty() || name.size() > 40) name = Wide(Stem(exe));
  return name;
}

void SaveClip(double end_ago = 0) {
  if (!g.session) return PlayChime(false);
  const ULONGLONG now = GetTickCount64();
  if (now - g.last_save < 1000) return;  // double press
  g.last_save = now;

  const std::wstring game = Wide(Stem(g.session->exe));
  const std::wstring title = g.session_title;
  const fs::path folder = ClipsFolder() / game;
  std::error_code ec;
  fs::create_directories(folder, ec);
  ULARGE_INTEGER free;
  if (GetDiskFreeSpaceExW(folder.c_str(), &free, nullptr, nullptr) &&
      free.QuadPart < uint64_t(g.cfg.min_free_gb) << 30) {
    PlayChime(false);
    return Balloon(L"Clip not saved", std::format(L"Less than {} GB free on the clips drive.", g.cfg.min_free_gb));
  }
  SYSTEMTIME t;
  GetLocalTime(&t);
  auto name = std::format(L"{} - Clipped with Clip - {:04}-{:02}-{:02} {:02}-{:02}-{:02}.mp4", title, t.wYear, t.wMonth,
                          t.wDay, t.wHour, t.wMinute, t.wSecond);
  const fs::path file = folder / name;
  g.session->Save(
      g.cfg.clip_seconds, file,
      [file](std::string err) { PostMessageW(g.wnd, WM_SAVED, 0, LPARAM(new Saved{std::move(err), file})); }, end_ago);
  // Feedback the moment the key is pressed; writing the file and the clipboard copy finish in the background.
  PlayChime(true);
  g.last_clip = file;
  SavedNotice(title + std::format(L" Â· last {} s", g.cfg.clip_seconds),
              g.cfg.copy_to_clipboard ? L"On your clipboard in a moment. Click to show in folder." : L"Click to show in folder.");
}

HGLOBAL GlobalText(const std::wstring& s) {
  HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (s.size() + 1) * sizeof(wchar_t));
  if (h) {
    memcpy(GlobalLock(h), s.c_str(), (s.size() + 1) * sizeof(wchar_t));
    GlobalUnlock(h);
  }
  return h;
}

HGLOBAL DropFiles(const fs::path& file) {
  const std::wstring path = file.wstring();
  HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + (path.size() + 2) * sizeof(wchar_t));
  if (h) {
    auto* df = static_cast<DROPFILES*>(GlobalLock(h));
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    memcpy(df + 1, path.c_str(), path.size() * sizeof(wchar_t));
    GlobalUnlock(h);
  }
  return h;
}

// The clip as a file, the way Explorer copies one. Discord takes only the file when there's also text, so the
// file is handed over when something pastes it (delayed rendering), and then the caption replaces it:
// Ctrl+V attaches the clip, the next Ctrl+V types "#ClippedWithClip on â€¦".
void CopyFileToClipboard(const fs::path& file, const std::wstring& caption) {
  if (!OpenClipboard(g.wnd)) return Log("can't copy clip to clipboard");
  EmptyClipboard();
  g.paste_file = file;
  g.paste_caption = caption;
  g.paste_since = GetTickCount64();
  SetClipboardData(CF_HDROP, nullptr);  // rendered in WM_RENDERFORMAT
  if (HGLOBAL effect = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD))) {
    *static_cast<DWORD*>(GlobalLock(effect)) = DROPEFFECT_COPY;
    GlobalUnlock(effect);
    if (!SetClipboardData(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT), effect)) GlobalFree(effect);
  }
  CloseClipboard();
}

bool DiscordInFront() {
  DWORD pid = 0;
  GetWindowThreadProcessId(GetForegroundWindow(), &pid);
  return pid && ExeName(pid).starts_with("discord");  // discord, discordptb, discordcanary
}

// Something asked for the clip: hand it over, then put the caption up. Requests in the first moment after copying
// come from clipboard watchers, not a paste, so they don't trigger it.
void RenderClipboardFile() {
  if (g.paste_file.empty()) return;
  if (HGLOBAL h = DropFiles(g.paste_file); h && !SetClipboardData(CF_HDROP, h)) GlobalFree(h);
  if (g.paste_caption.empty() || GetTickCount64() - g.paste_since <= 700) return;
  g.caption_into_discord = DiscordInFront();
  g.caption_waits = 0;
  SetTimer(g.wnd, kCaptionTimer, 250, nullptr);
}

// Discord keeps only the file when a paste carries both, so when the clip was pasted into Discord the caption is
// pasted right after it (one Ctrl+V gives both); anywhere else it waits on the clipboard for the next Ctrl+V.
void PutCaptionOnClipboard() {
  if ((GetAsyncKeyState(VK_CONTROL) < 0 || GetAsyncKeyState('V') < 0) && ++g.caption_waits < 30) return;  // still held
  KillTimer(g.wnd, kCaptionTimer);
  const std::wstring caption = g.paste_caption;  // EmptyClipboard sends us WM_DESTROYCLIPBOARD, which clears it
  if (caption.empty() || GetClipboardOwner() != g.wnd || !OpenClipboard(g.wnd)) return;
  EmptyClipboard();
  if (HGLOBAL t = GlobalText(caption); t && !SetClipboardData(CF_UNICODETEXT, t)) GlobalFree(t);
  CloseClipboard();
  if (!g.caption_into_discord || !DiscordInFront()) return;
  INPUT in[4]{};
  for (auto& i : in) i.type = INPUT_KEYBOARD;
  in[0].ki.wVk = in[3].ki.wVk = VK_CONTROL;
  in[1].ki.wVk = in[2].ki.wVk = 'V';
  in[2].ki.dwFlags = in[3].ki.dwFlags = KEYEVENTF_KEYUP;
  SendInput(4, in, sizeof(INPUT));
}

// ---- gallery and export ----

constexpr wchar_t kClipsHost[] = L"clips.example";  // the settings page plays clips from https://clips.example/...

fs::path SharedFolder() { return KnownFolder(FOLDERID_LocalAppData) / L"Clip" / L"Shared"; }

// A clip path from the page, only if it's an MP4 inside the clips folder.
std::optional<fs::path> ClipFromPage(const std::string& rel) {
  const fs::path root = ClipsFolder().lexically_normal();
  const fs::path p = (root / fs::path(Wide(rel))).lexically_normal();
  const auto [end, _] = std::mismatch(root.begin(), root.end(), p.begin(), p.end());
  std::error_code ec;
  if (end != root.end() || Lower(Utf8(p.extension().wstring())) != ".mp4" || !fs::is_regular_file(p, ec))
    return std::nullopt;
  return p;
}

// ---- previews: thumbnail, filmstrip and waveform, made once per clip and cached ----
constexpr wchar_t kCacheHost[] = L"cache.example";
fs::path CacheRoot() { return KnownFolder(FOLDERID_LocalAppData) / L"Clip" / L"cache"; }
// A folder per version of a clip: a rename or a re-save gets fresh previews.
fs::path CacheDir(const fs::path& clip) {
  std::error_code ec;
  uint64_t h = 1469598103934665603ull;
  auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
  for (unsigned char ch : Lower(Utf8(clip.wstring()))) mix(ch);
  mix(fs::file_size(clip, ec));
  mix(uint64_t(fs::last_write_time(clip, ec).time_since_epoch().count()));
  return CacheRoot() / std::format(L"{:016x}", h);
}
void ServeCache() {
  std::error_code ec;
  fs::create_directories(CacheRoot(), ec);
  SettingsServe(kCacheHost, CacheRoot());
}
std::string CacheUrl(const fs::path& dir, const wchar_t* file) {
  return Utf8(std::format(L"https://{}/{}/{}", kCacheHost, dir.filename().wstring(), file));
}


// One clip at a time at background priority, and not while a game records unless the window is open.
void PreviewLoop() {
  SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
  for (;;) {
    std::pair<fs::path, std::string> job;
    {
      std::unique_lock l(pv.m);
      for (;;) {
        if (pv.stop) return;
        if (!pv.q.empty() && (!pv.recording || SettingsVisible())) break;
        pv.cv.wait_for(l, std::chrono::seconds(2));
      }
      job = std::move(pv.q.front());
      pv.q.pop_front();
      pv.queued.erase(job.first);
    }
    std::error_code ec;
    const fs::path dir = CacheDir(job.first);
    if (!fs::exists(dir / L"strip.jpg", ec) && fs::exists(job.first, ec))
      if (const auto err = MakePreview(job.first, dir); !err.empty()) Log("preview {}: {}", Utf8(job.first.filename().wstring()), err);
    PostMessageW(g.wnd, WM_PREVIEW_READY, 0, LPARAM(new std::string(job.second)));
  }
}
void QueuePreview(const fs::path& clip, const std::string& rel, bool first) {
  std::lock_guard l(pv.m);
  if (!pv.thread.joinable()) pv.thread = std::thread(PreviewLoop);
  if (!pv.queued.insert(clip).second) return;
  if (first) pv.q.emplace_front(clip, rel);
  else pv.q.emplace_back(clip, rel);
  pv.cv.notify_one();
}
void StopPreviews() {
  {
    std::lock_guard l(pv.m);
    pv.stop = true;
  }
  pv.cv.notify_all();
  if (pv.thread.joinable()) pv.thread.join();
}
// Previews of clips untouched for two months go; they're remade if the clip is opened again.
void PruneCache() {
  std::error_code ec;
  const auto old = fs::file_time_type::clock::now() - std::chrono::hours(24 * 60);
  for (auto& e : fs::directory_iterator(CacheRoot(), ec))
    if (e.is_directory(ec) && e.last_write_time(ec) < old) fs::remove_all(e.path(), ec);
}
toml::table PreviewInfo(const fs::path& dir) {
  std::error_code ec;
  toml::table t;
  if (fs::exists(dir / L"thumb.jpg", ec)) t.insert("thumb", CacheUrl(dir, L"thumb.jpg"));
  if (fs::exists(dir / L"strip.jpg", ec))
    t.insert("strip", toml::table{{"url", CacheUrl(dir, L"strip.jpg")}, {"tiles", kStripTiles}, {"cols", kStripCols},
                                  {"w", kTileW}, {"h", kTileH}});
  if (std::ifstream f{dir / L"duration.txt"}) {  // "<seconds> <keyframes>"
    double dur = 0;
    int keys = 0;
    if (f >> dur) t.insert("duration", dur);
    if (f >> keys && t.contains("strip")) t["strip"].as_table()->insert("keys", keys);
  }
  return t;
}

void SendClips() {
  if (!SettingsWindow()) return;
  struct Item {
    std::string rel, game;
    int64_t size, ms;
  };
  std::vector<Item> items;
  const fs::path root = ClipsFolder();
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
       it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (it.depth() > 1) it.disable_recursion_pending();
    if (!it->is_regular_file(ec) || Lower(Utf8(it->path().extension().wstring())) != ".mp4") continue;
    const auto when = std::chrono::clock_cast<std::chrono::system_clock>(it->last_write_time(ec));
    items.push_back({Utf8(fs::relative(it->path(), root, ec).generic_wstring()),
                     Utf8(it->path().parent_path().filename().wstring()), int64_t(it->file_size(ec)),
                     std::chrono::duration_cast<std::chrono::milliseconds>(when.time_since_epoch()).count()});
  }
  std::ranges::sort(items, [](auto& a, auto& b) { return a.ms > b.ms; });
  toml::array list;
  for (auto& i : items) {  // clips sit in a folder per exe; the page shows the game's own name
    const auto known = FindGame(i.game);
    toml::table c{{"path", i.rel}, {"folder", i.game}, {"game", known ? known->name : i.game},
                  {"size", i.size}, {"time", i.ms}};
    const fs::path clip = root / Wide(i.rel);
    const toml::table p = PreviewInfo(CacheDir(clip));
    if (!p.contains("strip")) QueuePreview(clip, i.rel, false);  // newest first: the list is sorted
    for (auto& [k, v] : p) c.insert(k, v);
    list.push_back(std::move(c));
  }
  toml::table t{{"type", "clips"}, {"host", Utf8(std::wstring(L"https://") + kClipsHost + L"/")}, {"items", list}};
  std::stringstream ss;
  ss << toml::json_formatter{t};
  SettingsPost(ss.str());
}

// Quality presets for exports and clipboard copies: "original", "discord" (fits 10 MB), "1080" or "720".
void ApplyPreset(ExportOptions& o, const std::string& preset) {
  o.bitrate_mbps = g.cfg.bitrate_mbps;
  if (preset == "discord") o.target_bytes = 9'500'000;
  else if (preset == "1080") o.max_height = 1080, o.bitrate_mbps = std::min(g.cfg.bitrate_mbps, 12);
  else if (preset == "720") o.max_height = 720, o.bitrate_mbps = std::min(g.cfg.bitrate_mbps, 8);
}

// The editor's choices sent with an export: kept parts, audio track, vertical crop, watermark.
ExportOptions EditFromPage(const json::JsonObject& m) {
  ExportOptions o;
  if (m.HasKey(L"keep"))
    for (auto part : m.GetNamedArray(L"keep")) {
      auto a = part.GetArray();
      if (a.Size() == 2) o.keep.emplace_back(a.GetNumberAt(0), a.GetNumberAt(1));
    }
  o.audio = int(m.GetNamedNumber(L"audio", 0));
  if (m.HasKey(L"gains"))
    for (auto g : m.GetNamedArray(L"gains")) o.gains.push_back(float(std::clamp(g.GetNumber(), 0.0, 2.0)));
  o.vertical = m.GetNamedBoolean(L"vertical", false);
  o.crop_x = m.GetNamedNumber(L"cropX", 0.5);
  o.watermark = m.GetNamedBoolean(L"watermark", g.cfg.watermark);
  return o;
}
fs::path FreeName(const fs::path& folder, const std::wstring& stem) {
  std::error_code ec;
  fs::path p = folder / (stem + L".mp4");
  for (int i = 2; fs::exists(p, ec); i++) p = folder / std::format(L"{} ({}).mp4", stem, i);
  return p;
}

struct ExportJob {
  ExportOptions o;
  std::string page_path;    // the clip as the page names it; empty for a hotkey save's own copy
  bool copy = false;         // put the result on the clipboard
  std::wstring caption;      // pasted with it
  std::string err;
};

// Tells the page an export it asked for has finished; page_path lets it match its own request.
void ExportDoneToPage(const std::string& page_path, const std::string& err, bool copied, const std::string& file) {
  if (page_path.empty()) return;
  toml::table t{{"type", "exportDone"}, {"path", page_path}, {"error", err}, {"copied", copied}, {"file", file}};
  std::stringstream ss;
  ss << toml::json_formatter{t};
  SettingsPost(ss.str());
}

void StartExport(ExportJob job) {
  if (g.exporting) {
    return ExportDoneToPage(job.page_path, "Another export is still running.", false, "");
  }
  if (g.export_thread.joinable()) g.export_thread.join();
  g.exporting = true;
  g.export_cancel = false;
  g.export_thread = std::thread([job = std::move(job)]() mutable {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    job.err = ExportClip(job.o, [](double p) { PostMessageW(g.wnd, WM_EXPORT_PROGRESS, WPARAM(p * 100), 0); },
                         g.export_cancel);
    PostMessageW(g.wnd, WM_EXPORT_DONE, 0, LPARAM(new ExportJob(std::move(job))));
  });
}

void OnExportDone(std::unique_ptr<ExportJob> job) {
  g.exporting = false;
  if (g.export_thread.joinable()) g.export_thread.join();
  const bool ok = job->err.empty();
  if (ok && job->copy) CopyFileToClipboard(job->o.out, job->caption);
  if (ok && !job->page_path.empty()) PlayChime(true);  // a hotkey save already chimed when it was pressed
  ExportDoneToPage(job->page_path, job->err, ok && job->copy, ok ? Utf8(job->o.out.wstring()) : "");
  if (ok && !job->copy) {
    g.last_clip = job->o.out;
    SendClips();
  }
}

std::wstring Caption(const SYSTEMTIME& t) {
  return std::format(L"#ClippedWithClip on {:02}-{:02}-{:02} at {}:{:02} {}", t.wMonth, t.wDay, t.wYear % 100,
                     (t.wHour + 11) % 12 + 1, t.wMinute, t.wHour < 12 ? L"AM" : L"PM");
}

// When a clip was recorded: the time in its name ("â€¦ 2026-10-14 21-19-03"), else when the file was created.
SYSTEMTIME RecordedAt(const fs::path& clip) {
  SYSTEMTIME t{};
  const std::wstring stem = clip.stem().wstring();
  static const std::wregex stamp(LR"((\d{4})-(\d{2})-(\d{2}) (\d{2})-(\d{2})-(\d{2})(?: \(\d+\))?$)");
  if (std::wsmatch m; std::regex_search(stem, m, stamp)) {
    t.wYear = WORD(std::stoi(m[1])), t.wMonth = WORD(std::stoi(m[2])), t.wDay = WORD(std::stoi(m[3]));
    t.wHour = WORD(std::stoi(m[4])), t.wMinute = WORD(std::stoi(m[5])), t.wSecond = WORD(std::stoi(m[6]));
    return t;
  }
  WIN32_FILE_ATTRIBUTE_DATA fa;
  FILETIME local;
  if (!GetFileAttributesExW(clip.c_str(), GetFileExInfoStandard, &fa) ||
      !FileTimeToLocalFileTime(&fa.ftCreationTime, &local) || !FileTimeToSystemTime(&local, &t))
    GetLocalTime(&t);
  return t;
}

// The clipboard copy of a clip: watermarked and sized for Discord unless settings say otherwise.
void CopyClip(const fs::path& clip, ExportOptions edit, std::string page_path = {}) {
  const SYSTEMTIME t = RecordedAt(clip);
  const std::wstring caption = g.cfg.share_caption ? Caption(t) : L"";
  if (!edit.watermark && g.cfg.share_size == "original" && edit.keep.empty() && edit.audio == 0 && edit.gains.empty() && !edit.vertical) {
    CopyFileToClipboard(clip, caption);
    return ExportDoneToPage(page_path, "", true, Utf8(clip.wstring()));
  }
  std::error_code ec;
  fs::create_directories(SharedFolder(), ec);
  for (auto& e : fs::directory_iterator(SharedFolder(), ec))  // copies only need to outlive a paste
    if (e.last_write_time(ec) < fs::file_time_type::clock::now() - std::chrono::hours(24)) fs::remove(e.path(), ec);
  ExportJob job;
  job.o = std::move(edit);
  job.o.in = clip;
  job.o.out = FreeName(SharedFolder(), clip.stem().wstring());
  ApplyPreset(job.o, g.cfg.share_size);
  job.copy = true;
  job.page_path = std::move(page_path);
  job.caption = caption;
  StartExport(std::move(job));
}

void RecycleFile(const fs::path& p) {
  std::wstring from = p.wstring() + L'\0';  // double-null terminated
  SHFILEOPSTRUCTW op{};
  op.wFunc = FO_DELETE;
  op.pFrom = from.c_str();
  op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
  if (SHFileOperationW(&op) != 0) Log("couldn't move {} to the Recycle Bin", Utf8(p.wstring()));
}

void OnGalleryAction(const std::string& name, const json::JsonObject& m) {
  if (name == "listClips") return SendClips();
  if (name == "cancelExport") {
    g.export_cancel = true;
    return;
  }
  const auto clip = ClipFromPage(winrt::to_string(m.GetNamedString(L"path", L"")));
  if (!clip) {  // gone or not a clip: refresh the page's list
    if (name == "exportClip")
      ExportDoneToPage(winrt::to_string(m.GetNamedString(L"path", L"")), "This clip is gone.", false, "");
    return SendClips();
  }
  if (name == "showClip") {
    g.last_clip = *clip;
    ShowLastClip();
  } else if (name == "deleteClip") {
    RecycleFile(*clip);
    SendClips();
  } else if (name == "renameClip") {
    std::wstring stem = Wide(winrt::to_string(m.GetNamedString(L"name", L"")));
    std::erase_if(stem, [](wchar_t c) { return c < 32 || std::wcschr(L"<>:\"/\\|?*", c); });
    std::error_code ec;
    if (!stem.empty() && !fs::exists(clip->parent_path() / (stem + L".mp4"), ec))
      fs::rename(*clip, clip->parent_path() / (stem + L".mp4"), ec);
    SendClips();
  } else if (name == "exportClip") {
    const std::string page_path = winrt::to_string(m.GetNamedString(L"path", L""));
    if (m.GetNamedBoolean(L"copy", false)) return CopyClip(*clip, EditFromPage(m), page_path);
    ExportJob job;
    job.page_path = page_path;
    job.o = EditFromPage(m);
    job.o.in = *clip;
    job.o.out = FreeName(ClipsFolder() / L"Exports", clip->stem().wstring());
    ApplyPreset(job.o, winrt::to_string(m.GetNamedString(L"preset", L"original")));
    StartExport(std::move(job));
  } else if (name == "clipInfo") {
    const std::string rel = winrt::to_string(m.GetNamedString(L"path", L""));
    const fs::path dir = CacheDir(*clip);
    std::error_code ec;
    if (!fs::exists(dir / L"strip.jpg", ec)) QueuePreview(*clip, rel, true);  // the open clip jumps the queue
    if (std::ifstream f{dir / L"info.json", std::ios::binary}) {  // waveform read before: answer at once
      std::string json{std::istreambuf_iterator<char>(f), {}};
      if (!json.empty()) return SettingsPost(json);
    }
    std::thread([path = *clip, rel, dir] {
      const ClipInfo info = ReadClipInfo(path);
      std::string peaks;
      peaks.reserve(info.peaks.size() * 5);
      for (float v : info.peaks) peaks += std::format("{:.3g},", v);
      if (!peaks.empty()) peaks.pop_back();
      toml::array tracks;
      for (auto& t : info.tracks) tracks.push_back(t);
      toml::table t{{"type", "clipInfo"}, {"path", rel}, {"duration", info.duration}, {"width", info.width},
                    {"height", info.height}, {"tracks", tracks}, {"rate", kPeaksPerSecond}, {"error", info.error}};
      std::stringstream ss;
      ss << toml::json_formatter{t};
      std::string json = ss.str();
      json.insert(json.rfind('}'), ",\"peaks\":[" + peaks + "]");
      if (info.error.empty()) {
        std::error_code ec;
        fs::create_directories(dir, ec);
        std::ofstream(dir / L"info.json", std::ios::binary | std::ios::trunc) << json;
      }
      PostMessageW(g.wnd, WM_CLIP_INFO, 0, LPARAM(new std::string(std::move(json))));
    }).detach();
  }
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
  if (id >= ID_PRESET && id - ID_PRESET < OverlayPresets().size())
    return ChangeConfig([&](Config& c) { c.overlay.keys = OverlayPresets()[id - ID_PRESET].keys; });
  if (id >= ID_LENGTH && id - ID_LENGTH < std::size(kLengths))
    return ChangeConfig([&](Config& c) { c.clip_seconds = kLengths[id - ID_LENGTH]; });
  switch (id) {
    case ID_SAVE: return SaveClip();
    case ID_SETTINGS: return OpenSettings();
    case ID_SHORTCUT: return ShowShortcutPicker();
    case ID_GAME_AUDIO: return ChangeConfig([](Config& c) { c.record_game_audio = !c.record_game_audio; });
    case ID_MIC: return ChangeConfig([](Config& c) { c.record_mic = !c.record_mic; });
    case ID_COPY: return ChangeConfig([](Config& c) { c.copy_to_clipboard = !c.copy_to_clipboard; });
    case ID_OVERLAY: return ChangeConfig([](Config& c) { c.overlay.enabled = !c.overlay.enabled; });
    case ID_OVERLAY_EDIT:
      return ShowOverlayEditor(g.cfg.overlay, [](const OverlayConfig& o) {
        ChangeConfig([&](Config& c) {
          c.overlay = o;
          c.overlay.enabled = true;  // you edited it to see it
        });
      });
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
  std::wstring status = g.paused ? L"Paused" : g.session ? L"Recording " + g.session_title : L"Waiting for a game";
  AppendMenuW(m, MF_STRING | MF_GRAYED, 0, status.c_str());
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, ID_SETTINGS, L"Settingsâ€¦");
  SetMenuDefaultItem(m, ID_SETTINGS, FALSE);
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
    AppendMenuW(m, MF_STRING, ID_NEVER, (L"Never record " + g.session_title).c_str());
  else if (!g.last_app_exe.empty() && IsWindow(g.last_app_wnd))
    AppendMenuW(m, MF_STRING, ID_ALWAYS, (L"Always record " + Wide(g.last_app_exe)).c_str());
  AppendMenuW(m, check(g.paused), ID_PAUSE, L"Pause recording");
  HMENU overlay = CreatePopupMenu();
  AppendMenuW(overlay, check(c.overlay.enabled), ID_OVERLAY, L"Show keys and mouse in clips");
  AppendMenuW(overlay, MF_SEPARATOR, 0, nullptr);
  for (UINT i = 0; i < OverlayPresets().size(); i++)
    AppendMenuW(overlay, check(c.overlay.keys == OverlayPresets()[i].keys), ID_PRESET + i,
                Wide(OverlayPresets()[i].name).c_str());
  AppendMenuW(overlay, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(overlay, MF_STRING, ID_OVERLAY_EDIT, L"Edit layout and lookâ€¦");
  AppendMenuW(m, MF_POPUP, UINT_PTR(overlay), L"Input overlay");
  AppendMenuW(m, check(c.copy_to_clipboard), ID_COPY, L"Copy new clips to clipboard");
  AppendMenuW(m, MF_STRING, ID_SHORTCUT, (L"Change shortcut (" + Wide(c.hotkey) + L")â€¦").c_str());
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

// ---- settings window ----

void SettingsPush() {
  if (!SettingsWindow()) return;
  SettingsServe(kClipsHost, ClipsFolder());  // follows a changed clips folder
  ServeCache();
  const OverlayConfig& ov = g.cfg.overlay;
  toml::array presets;
  int preset = -1;
  for (size_t i = 0; i < OverlayPresets().size(); i++) {
    presets.push_back(OverlayPresets()[i].name);
    if (ov.keys == OverlayPresets()[i].keys) preset = int(i);
  }
  const char* state = g.paused ? "paused" : g.session ? "recording" : "waiting";
  toml::table t{{"type", "state"},
                {"config", ConfigTable(g.cfg)},
                {"status", toml::table{{"state", state}, {"game", g.session ? Utf8(g.session_title) : ""}}},
                {"hotkey_error", g.hotkey_error},
                {"audio_apps", TomlList(AudioApps())},
                {"presets", std::move(presets)},
                {"preset", preset},
                {"folder", Utf8(ClipsFolder().wstring())},
                {"discord_available", kDiscordAppId[0] != 0},
                {"voice_error", g.voice_error}};
  std::stringstream ss;
  ss << toml::json_formatter{t};
  SettingsPost(ss.str());
}

void SettingsSay(const std::string& type, const char* key, const std::string& value) {
  toml::table t{{"type", type}, {key, value}};
  std::stringstream ss;
  ss << toml::json_formatter{t};
  SettingsPost(ss.str());
}

// One setting from the page, run through ParseConfig so it's checked and clamped like a file edit.
void SetFromPage(const std::string& key, const json::IJsonValue& v) {
  if (key == "overlay.preset") {
    const size_t i = size_t(v.GetNumber());
    if (i < OverlayPresets().size()) ChangeConfig([&](Config& c) { c.overlay.keys = OverlayPresets()[i].keys; });
    return;
  }
  toml::table t = ConfigTable(g.cfg);
  toml::table* where = &t;
  std::string k = key;
  if (key.starts_with("overlay.")) {
    where = t["overlay"].as_table();
    k = key.substr(8);
  }
  switch (v.ValueType()) {
    case json::JsonValueType::Boolean: where->insert_or_assign(k, v.GetBoolean()); break;
    case json::JsonValueType::Number: where->insert_or_assign(k, int64_t(std::llround(v.GetNumber()))); break;
    case json::JsonValueType::String: where->insert_or_assign(k, winrt::to_string(v.GetString())); break;
    case json::JsonValueType::Array: {
      toml::array a;
      for (auto e : v.GetArray())
        if (e.ValueType() == json::JsonValueType::String) a.push_back(winrt::to_string(e.GetString()));
      where->insert_or_assign(k, std::move(a));
      break;
    }
    default: return;
  }
  std::stringstream ss;
  ss << t;
  try {
    g.cfg = ParseConfig(ss.str());
  } catch (const toml::parse_error& e) {
    return Log("settings: {} not applied: {}", key, e.description());
  }
  SaveConfig();
  ApplyConfig();
}

void StopTapPicker() {
  if (g.tap_picker) raw_input::Unlisten(std::exchange(g.tap_picker, 0));
}

// Waits for any key Windows can see to be tapped twice, for the double-tap shortcut.
void StartTapPicker() {
  StopTapPicker();
  struct Taps {
    std::string last;
    int64_t at = 0;
    bool held = false;
  };
  g.tap_picker = raw_input::Listen([t = std::make_shared<Taps>()](const RAWINPUT& ri) {
    if (ri.header.dwType != RIM_TYPEKEYBOARD) return;
    const std::string name = RawKeyName(ri.data.keyboard);
    if (name.empty()) return;
    if (ri.data.keyboard.Flags & RI_KEY_BREAK) {
      if (name == t->last) t->held = false;
      return;
    }
    if (name == t->last && t->held) return;  // auto-repeat
    const int64_t now = QpcNow();
    if (name == t->last && now - t->at < QpcFreq() * 400 / 1000) {
      PostMessageW(g.wnd, WM_TAP_PICKED, 0, LPARAM(new std::string(name)));
      t->last.clear();
    } else {
      t->last = name;
      t->at = now;
    }
    t->held = true;
  });
}

void BrowseFolder() {
  try {
    auto dlg = winrt::create_instance<IFileOpenDialog>(CLSID_FileOpenDialog);
    FILEOPENDIALOGOPTIONS o;
    dlg->GetOptions(&o);
    dlg->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(L"Save clips to");
    com_ptr<IShellItem> start;
    std::error_code ec;
    fs::create_directories(ClipsFolder(), ec);
    if (SUCCEEDED(SHCreateItemFromParsingName(ClipsFolder().c_str(), nullptr, IID_PPV_ARGS(start.put()))))
      dlg->SetFolder(start.get());
    if (dlg->Show(SettingsWindow()) != S_OK) return;
    com_ptr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dlg->GetResult(item.put())) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return;
    const std::string folder = Utf8(path);
    CoTaskMemFree(path);
    ChangeConfig([&](Config& c) { c.clips_folder = folder; });
  } catch (const winrt::hresult_error& e) {
    Log("folder picker: {}", winrt::to_string(e.message()));
  }
}

void OnSettingsMessage(const std::string& text) {
  json::JsonObject m{nullptr};
  if (!json::JsonObject::TryParse(Wide(text), m)) return;
  const std::string type = winrt::to_string(m.GetNamedString(L"type", L""));
  if (type == "ready") return SettingsPush();
  if (type == "shown") return SendClips();  // reopened: catch up on clips saved while hidden
  if (type == "log") return Log("page: {}", winrt::to_string(m.GetNamedString(L"text", L"")));
  if (type == "set") return SetFromPage(winrt::to_string(m.GetNamedString(L"key", L"")), m.GetNamedValue(L"value"));
  if (type != "action") return;
  const std::string name = winrt::to_string(m.GetNamedString(L"name", L""));
  if (name == "pickHotkey") {
    UnregisterHotKey(g.wnd, kHotkey);  // so pressing the current shortcut reaches the page
    g.hotkey.clear();
    g.page_picking = true;
  } else if (name == "hotkeyKey") {
    const std::wstring err = TryShortcut(UINT(m.GetNamedNumber(L"vk", 0)), m.GetNamedBoolean(L"ctrl", false),
                                         m.GetNamedBoolean(L"alt", false), m.GetNamedBoolean(L"shift", false),
                                         m.GetNamedBoolean(L"win", false));
    if (err != L"â€¦") SettingsSay("hotkeyResult", "error", Utf8(err));
  } else if (name == "cancelHotkey") {
    g.page_picking = false;
    g.hotkey.clear();
    ApplyConfig();
  } else if (name == "pickDoubleTap") {
    StartTapPicker();
  } else if (name == "cancelDoubleTap") {
    StopTapPicker();
  } else if (name == "browseFolder") {
    BrowseFolder();
  } else if (name == "openFolder") {
    OnCommand(ID_OPEN_FOLDER);
  } else if (name == "editOverlay") {
    OnCommand(ID_OVERLAY_EDIT);
  } else if (name == "togglePause") {
    OnCommand(ID_PAUSE);
  } else if (name == "saveClip") {
    SaveClip();
  } else if (name == "showLast") {
    ShowLastClip();
  } else if (name == "neverRecord") {
    OnCommand(ID_NEVER);
  } else {
    OnGalleryAction(name, m);
  }
}

void OpenSettings() {
  ShowSettings(OnSettingsMessage);
  SettingsServe(kClipsHost, ClipsFolder());
  ServeCache();
}

// ---- window ----

const UINT WM_TASKBAR_CREATED = RegisterWindowMessageW(L"TaskbarCreated");
const UINT WM_SHOW_CLIP = RegisterWindowMessageW(L"Clip.Show");  // Clip was launched again

LRESULT CALLBACK WndProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_TASKBAR_CREATED) {  // Explorer restarted
    AddTrayIcon();
    UpdateTip();
    return 0;
  }
  if (msg == WM_SHOW_CLIP) return OpenSettings(), 0;
  switch (msg) {
    case WM_TRAY:
      if (lp == WM_LBUTTONUP) OpenSettings();
      else if (lp == WM_RBUTTONUP) ShowMenu();
      else if (lp == NIN_BALLOONUSERCLICK && g.balloon_shows_clip) ShowLastClip();
      return 0;
    case WM_EXPORT_PROGRESS:
      SettingsPost(std::format(R"({{"type":"exportProgress","p":{}}})", int(wp)));
      return 0;
    case WM_GAMES_READY:  // pretty names and icons for whatever is showing
      if (g.session) {
        g.session_title = GameName(g.session->pid, g.session->exe);
        UpdateTip();
      }
      if (g.discord) g.discord->Refresh();
      SyncDiscord();
      SettingsPush();
      SendClips();
      return 0;
    case WM_PREVIEW_READY: {
      std::unique_ptr<std::string> rel(reinterpret_cast<std::string*>(lp));
      if (!SettingsWindow()) return 0;
      toml::table t = PreviewInfo(CacheDir(ClipsFolder() / Wide(*rel)));
      t.insert("type", "preview");
      t.insert("path", *rel);
      std::stringstream ss;
      ss << toml::json_formatter{t};
      SettingsPost(ss.str());
      return 0;
    }
    case WM_CLIP_INFO: {
      std::unique_ptr<std::string> json(reinterpret_cast<std::string*>(lp));
      SettingsPost(*json);
      return 0;
    }
    case WM_EXPORT_DONE: return OnExportDone(std::unique_ptr<ExportJob>(reinterpret_cast<ExportJob*>(lp))), 0;
    case WM_TAP_PICKED: {
      std::unique_ptr<std::string> key(reinterpret_cast<std::string*>(lp));
      if (!g.tap_picker) return 0;
      StopTapPicker();
      ChangeConfig([&](Config& c) { c.double_tap = *key; });
      SettingsSay("tapPicked", "key", *key);
      PlaySoundW(L"SystemAsterisk", nullptr, SND_ALIAS | SND_ASYNC);
      return 0;
    }
    case WM_HOTKEY: SaveClip(); return 0;
    case WM_VOICE_SAVE:  // the clip ends where the phrase began
      Log("voice: heard the phrase");
      SaveClip(double(wp) / 1000);
      return 0;
    case WM_VOICE_FAIL: {
      std::unique_ptr<std::string> why(reinterpret_cast<std::string*>(lp));
      Log("voice: {}", *why);
      g.voice_error = *why;
      SettingsPush();
      return 0;
    }
    case WM_RENDERFORMAT:
      if (wp == CF_HDROP) RenderClipboardFile();
      return 0;
    case WM_RENDERALLFORMATS:  // quitting while the clipboard still waits on us
      if (OpenClipboard(w)) {
        if (GetClipboardOwner() == w) RenderClipboardFile();
        CloseClipboard();
      }
      KillTimer(w, kCaptionTimer);
      return 0;
    case WM_DESTROYCLIPBOARD:  // someone else copied something
      if (!g.paste_file.empty()) g.paste_file.clear(), g.paste_caption.clear();
      return 0;
    case WM_TIMER:
      if (wp == kCaptionTimer) return PutCaptionOnClipboard(), 0;
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
      std::unique_ptr<Saved> s(reinterpret_cast<Saved*>(lp));
      if (s->err.empty()) {  // the chime and notice already went out when the key was pressed
        g.last_clip = s->file;
        if (g.cfg.copy_to_clipboard) CopyClip(s->file, {.watermark = g.cfg.watermark});
        std::error_code ec;
        QueuePreview(s->file, Utf8(fs::relative(s->file, ClipsFolder(), ec).generic_wstring()), true);
        SettingsSay("saved", "file", Utf8(s->file.filename().wstring()));
        SendClips();
      } else {
        PlayChime(false);
        Balloon(L"Clip not saved", Wide(s->err));
        SettingsSay("saveFailed", "error", s->err);
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

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmd, int) {
  const bool background = cmd && wcsstr(cmd, L"--background");  // started at login
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);  // real pixels for the fullscreen test
  if (!IsWindows11()) {
    MessageBoxW(nullptr, L"Clip needs Windows 11.", L"Clip", MB_ICONERROR);
    return 1;
  }
  CreateMutexW(nullptr, TRUE, L"Local\\Clip.SingleInstance");
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    if (HWND running = FindWindowW(L"ClipTray", nullptr)) PostMessageW(running, WM_SHOW_CLIP, 0, 0);
    return 0;
  }
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
  LoadGameList([] { PostMessageW(g.wnd, WM_GAMES_READY, 0, 0); });
  EnsureStartMenuShortcut();
  if (!background) OpenSettings();

  HWINEVENTHOOK fg_hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, OnWinEvent, 0, 0,
                                          WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
  HANDLE watch =
      FindFirstChangeNotificationW(cfg_dir.c_str(), FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME);
  const DWORD n_handles = watch != INVALID_HANDLE_VALUE ? 1 : 0;
  CheckForeground();
  std::thread(PruneCache).detach();

  for (MSG msg;;) {
    const DWORD r = MsgWaitForMultipleObjects(n_handles, &watch, FALSE, INFINITE, QS_ALLINPUT);
    if (n_handles && r == WAIT_OBJECT_0) {
      FindNextChangeNotification(watch);
      SetTimer(g.wnd, kReloadTimer, 300, nullptr);  // editors write in bursts
      continue;
    }
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) {
        g.export_cancel = true;
        if (g.export_thread.joinable()) g.export_thread.join();
        StopPreviews();
        g.discord.reset();
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
