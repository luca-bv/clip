#include <unknwn.h>  // before C++/WinRT, so com_ptr works with classic COM

#include "settings.h"

#include <WebView2.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <winrt/base.h>
#include <wrl.h>

#include <filesystem>
#include <fstream>
#include <map>

using Microsoft::WRL::Callback;
using winrt::com_ptr;

namespace {
struct {
  HWND wnd = nullptr;
  com_ptr<ICoreWebView2Controller> controller;
  com_ptr<ICoreWebView2> view;
  std::function<void(const std::string&)> on_message;
  std::map<std::wstring, std::filesystem::path> want, served;  // host -> folder: asked for, and what the view has
} s;

constexpr UINT_PTR kForgetTimer = 1;
constexpr UINT kKeepHiddenMs = 10 * 60 * 1000;  // a closed window stays ready this long, then frees its memory

void ApplyServe() {
  auto v3 = s.view ? s.view.try_as<ICoreWebView2_3>() : nullptr;
  if (!v3) return;
  for (auto& [host, folder] : s.want) {
    if (auto it = s.served.find(host); it != s.served.end()) {
      if (it->second == folder) continue;
      v3->ClearVirtualHostNameToFolderMapping(host.c_str());
    }
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    if (SUCCEEDED(v3->SetVirtualHostNameToFolderMapping(host.c_str(), folder.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW)))
      s.served[host] = folder;
  }
}

std::filesystem::path AppData(const wchar_t* sub) {
  PWSTR p = nullptr;
  std::filesystem::path r;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) r = std::filesystem::path(p) / L"Clip" / sub;
  CoTaskMemFree(p);
  return r;
}

// The page and its libraries live in Clip.exe (clip.rc) and are served from this folder as https://clip.example/.
constexpr struct {
  int id;
  const wchar_t* path;
} kUiFiles[] = {{100, L"index.html"},         {101, L"vendor/wavesurfer.min.js"}, {102, L"vendor/regions.min.js"},
                {103, L"vendor/timeline.min.js"}, {104, L"vendor/zoom.min.js"},   {105, L"vendor/hover.min.js"}};

std::filesystem::path WriteUi() {
  const auto dir = AppData(L"ui");
  for (auto& f : kUiFiles) {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(f.id), MAKEINTRESOURCEW(10));  // RT_RCDATA
    if (!r) continue;
    const auto* data = static_cast<const char*>(LockResource(LoadResource(nullptr, r)));
    const auto path = dir / f.path;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream(path, std::ios::binary | std::ios::trunc).write(data, SizeofResource(nullptr, r));
  }
  return dir;
}

void Fit() {
  if (!s.controller) return;
  RECT r;
  GetClientRect(s.wnd, &r);
  s.controller->put_Bounds(r);
}

void OnController(ICoreWebView2Controller* c) {
  s.controller.copy_from(c);
  s.controller->get_CoreWebView2(s.view.put());
  // The page's background, so there's no flash while it loads.
  if (auto c2 = s.controller.try_as<ICoreWebView2Controller2>()) c2->put_DefaultBackgroundColor({255, 0x12, 0x12, 0x12});
  com_ptr<ICoreWebView2Settings> st;
  if (SUCCEEDED(s.view->get_Settings(st.put()))) {
    st->put_AreDefaultContextMenusEnabled(FALSE);
    st->put_AreDevToolsEnabled(FALSE);
    st->put_IsStatusBarEnabled(FALSE);
    st->put_IsZoomControlEnabled(FALSE);
    if (auto st3 = st.try_as<ICoreWebView2Settings3>()) st3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
  }
  EventRegistrationToken token;
  s.view->add_WebMessageReceived(
      Callback<ICoreWebView2WebMessageReceivedEventHandler>(
          [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
            LPWSTR json = nullptr;
            if (SUCCEEDED(args->get_WebMessageAsJson(&json))) {
              const std::string m = Utf8(json);
              CoTaskMemFree(json);
              if (s.on_message) s.on_message(m);
            }
            return S_OK;
          })
          .Get(),
      &token);
  Fit();
  s.served.clear();
  ApplyServe();
  if (auto v3 = s.view.try_as<ICoreWebView2_3>())
    v3->SetVirtualHostNameToFolderMapping(L"clip.example", WriteUi().c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
  s.view->Navigate(L"https://clip.example/index.html");
}

void StartWebView() {
  // Audio track switching, so the editor can play each of a clip's tracks on its own.
  SetEnvironmentVariableW(L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", L"--enable-blink-features=AudioVideoTracks");
  const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
      nullptr, AppData(L"WebView2").c_str(), nullptr,
      Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
          [](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
            if (!s.wnd) return S_OK;
            if (FAILED(hr) || !env) {
              Log("settings: WebView2 unavailable (0x{:08x})", uint32_t(hr));
              return S_OK;
            }
            return env->CreateCoreWebView2Controller(
                s.wnd, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                           [](HRESULT hr, ICoreWebView2Controller* c) -> HRESULT {
                             if (s.wnd && SUCCEEDED(hr) && c) OnController(c);
                             else if (FAILED(hr)) Log("settings: no WebView2 controller (0x{:08x})", uint32_t(hr));
                             return S_OK;
                           })
                           .Get());
          })
          .Get());
  if (FAILED(hr)) {
    Log("settings: WebView2 runtime missing (0x{:08x})", uint32_t(hr));
    MessageBoxW(s.wnd, L"Clip's settings need the Microsoft Edge WebView2 Runtime, which comes with Windows 11. "
                       L"Reinstall it from microsoft.com/edge/webview2.",
                L"Clip", MB_ICONERROR);
    DestroyWindow(s.wnd);
  }
}

LRESULT CALLBACK Proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_SIZE: Fit(); return 0;
    case WM_GETMINMAXINFO: {
      const UINT dpi = GetDpiForWindow(w);
      auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
      mm->ptMinTrackSize = {MulDiv(560, dpi, 96), MulDiv(380, dpi, 96)};
      return 0;
    }
    case WM_DPICHANGED: {
      const auto* r = reinterpret_cast<const RECT*>(lp);
      SetWindowPos(w, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      return 0;
    }
    case WM_CLOSE:  // hide, so opening it again is instant; the page pauses itself
      ShowWindow(w, SW_HIDE);
      SettingsPost(R"({"type":"hidden"})");
      SetTimer(w, kForgetTimer, kKeepHiddenMs, nullptr);
      return 0;
    case WM_TIMER:
      if (wp == kForgetTimer && !IsWindowVisible(w)) DestroyWindow(w);
      return 0;
    case WM_DESTROY:
      if (s.controller) s.controller->Close();
      s.controller = nullptr;
      s.view = nullptr;
      s.wnd = nullptr;
      return 0;
  }
  return DefWindowProcW(w, msg, wp, lp);
}
}  // namespace

void ShowSettings(std::function<void(const std::string&)> on_message) {
  s.on_message = std::move(on_message);
  if (s.wnd) {
    KillTimer(s.wnd, kForgetTimer);
    if (!IsWindowVisible(s.wnd)) {
      ShowWindow(s.wnd, SW_SHOWNORMAL);
      SettingsPost(R"({"type":"shown"})");
    } else if (IsIconic(s.wnd)) {
      ShowWindow(s.wnd, SW_RESTORE);
    }
    SetForegroundWindow(s.wnd);
    return;
  }
  HINSTANCE inst = GetModuleHandleW(nullptr);
  static const bool registered = [inst] {
    WNDCLASSW wc{};
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"ClipSettings";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hbrBackground = CreateSolidBrush(RGB(0x12, 0x12, 0x12));
    return RegisterClassW(&wc) != 0;
  }();
  (void)registered;
  const UINT dpi = GetDpiForSystem();
  s.wnd = CreateWindowExW(0, L"ClipSettings", L"Clip", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                          MulDiv(980, dpi, 96), MulDiv(660, dpi, 96), nullptr, nullptr, inst, nullptr);
  const BOOL dark = TRUE;  // Clip is always dark
  DwmSetWindowAttribute(s.wnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof dark);
  ShowWindow(s.wnd, SW_SHOWNORMAL);
  SetForegroundWindow(s.wnd);
  StartWebView();
}

void SettingsPost(const std::string& json) {
  if (s.view) s.view->PostWebMessageAsJson(Wide(json).c_str());
}

HWND SettingsWindow() { return s.wnd; }

void SettingsServe(const std::wstring& host, const std::filesystem::path& folder) {
  s.want[host] = folder;
  ApplyServe();
}

bool SettingsVisible() { return s.wnd && IsWindowVisible(s.wnd); }

void SettingsForgetHidden() {
  if (s.wnd && !IsWindowVisible(s.wnd)) DestroyWindow(s.wnd);
}
