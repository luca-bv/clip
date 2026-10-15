#include "input.h"

#include <future>
#include <map>
#include <mutex>
#include <thread>

namespace raw_input {
namespace {
std::mutex life_m;  // Listen/Unlisten, so a thread never stops while a new one starts
std::mutex m;       // listeners
std::map<int, std::function<void(const RAWINPUT&)>> listeners;
int next_id = 1;
std::thread thread;
DWORD thread_id = 0;

void Loop(std::promise<void>* started) {
  HINSTANCE inst = GetModuleHandleW(nullptr);
  WNDCLASSW wc{};
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = inst;
  wc.lpszClassName = L"ClipInput";
  RegisterClassW(&wc);
  HWND w = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
  RAWINPUTDEVICE devs[] = {{0x01, 0x06, RIDEV_INPUTSINK, w}, {0x01, 0x02, RIDEV_INPUTSINK, w}};  // keyboard, mouse
  if (!w || !RegisterRawInputDevices(devs, 2, sizeof devs[0])) Log("raw input: not available ({})", GetLastError());
  thread_id = GetCurrentThreadId();
  started->set_value();

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (msg.message == WM_INPUT) {
      RAWINPUT ri;
      UINT size = sizeof ri;
      if (GetRawInputData(HRAWINPUT(msg.lParam), RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) != UINT(-1)) {
        std::lock_guard l(m);
        for (auto& [id, f] : listeners) f(ri);
      }
    }
    DispatchMessageW(&msg);
  }
  for (auto& d : devs) {
    d.dwFlags = RIDEV_REMOVE;
    d.hwndTarget = nullptr;
  }
  RegisterRawInputDevices(devs, 2, sizeof devs[0]);
  if (w) DestroyWindow(w);
}
}  // namespace

int Listen(std::function<void(const RAWINPUT&)> f) {
  std::lock_guard life(life_m);
  int id;
  bool start;
  {
    std::lock_guard l(m);
    id = next_id++;
    start = listeners.empty();
    listeners.emplace(id, std::move(f));
  }
  if (start) {
    std::promise<void> started;
    auto ready = started.get_future();
    thread = std::thread(Loop, &started);
    ready.wait();
  }
  return id;
}

void Unlisten(int id) {
  std::lock_guard life(life_m);
  bool stop;
  {
    std::lock_guard l(m);
    stop = listeners.erase(id) && listeners.empty();
  }
  if (stop && thread.joinable()) {
    PostThreadMessageW(thread_id, WM_QUIT, 0, 0);
    thread.join();
  }
}
}  // namespace raw_input

std::string RawKeyName(const RAWKEYBOARD& k) {
  const bool e0 = k.Flags & RI_KEY_E0;
  switch (k.VKey) {
    case VK_CONTROL: return e0 ? "RCtrl" : "LCtrl";
    case VK_MENU: return e0 ? "RAlt" : "LAlt";
    case VK_SHIFT: return k.MakeCode == 0x36 ? "RShift" : "LShift";
    case VK_CAPITAL: return "Caps";
    case VK_SPACE: return "Space";
    case VK_TAB: return "Tab";
    case VK_RWIN: return "RWin";
    case VK_APPS: return "Menu";
    case VK_SCROLL: return "ScrollLock";
    case VK_PAUSE: return "Pause";
    case VK_INSERT: return "Insert";
    case 0xFF: return k.MakeCode ? std::format("Scan{:02X}", k.MakeCode) : "";  // vendor keys with no virtual key
  }
  if ((k.VKey >= 'A' && k.VKey <= 'Z') || (k.VKey >= '0' && k.VKey <= '9')) return std::string(1, char(k.VKey));
  if (k.VKey >= VK_F1 && k.VKey <= VK_F24) return "F" + std::to_string(k.VKey - VK_F1 + 1);
  return k.VKey ? std::format("Key{:02X}", k.VKey) : "";
}

DoubleTap::DoubleTap(std::string key, std::function<void()> fire)
    : key_(std::move(key)), fire_(std::move(fire)), id_(raw_input::Listen([this](auto& ri) { OnInput(ri); })) {}

DoubleTap::~DoubleTap() { raw_input::Unlisten(id_); }

void DoubleTap::OnInput(const RAWINPUT& ri) {
  if (ri.header.dwType != RIM_TYPEKEYBOARD) return;
  const std::string name = RawKeyName(ri.data.keyboard);
  if (ri.data.keyboard.Flags & RI_KEY_BREAK) {
    if (name == held_) held_.clear();
    return;
  }
  if (name == held_) return;  // auto-repeat
  held_ = name;
  if (name != key_) {
    last_tap_ = 0;  // another key in between: not a double tap
    return;
  }
  const int64_t now = QpcNow();
  if (last_tap_ && now - last_tap_ < QpcFreq() * 350 / 1000) {
    last_tap_ = 0;
    fire_();
  } else {
    last_tap_ = now;
  }
}
