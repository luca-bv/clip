#include "overlay.h"

#include <algorithm>

namespace {
const std::pair<const char*, UINT> kNamedKeys[] = {
    {"Space", VK_SPACE}, {"Shift", VK_SHIFT},    {"Ctrl", VK_CONTROL},  {"Alt", VK_MENU},      {"Tab", VK_TAB},
    {"Enter", VK_RETURN}, {"Esc", VK_ESCAPE},    {"Caps", VK_CAPITAL},  {"Backspace", VK_BACK}, {"Up", VK_UP},
    {"Down", VK_DOWN},   {"Left", VK_LEFT},      {"Right", VK_RIGHT},   {"Insert", VK_INSERT}, {"Home", VK_HOME},
    {"End", VK_END},     {"PageUp", VK_PRIOR},   {"PageDown", VK_NEXT}};
const char* const kMouseNames[] = {"LMB", "RMB", "MMB", "Mouse4", "Mouse5", "WheelUp", "WheelDown", "Mouse"};
const struct {
  USHORT down, up;
  const char* name;
} kButtons[] = {{RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP, "LMB"},
                {RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP, "RMB"},
                {RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, "MMB"},
                {RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_4_UP, "Mouse4"},
                {RI_MOUSE_BUTTON_5_DOWN, RI_MOUSE_BUTTON_5_UP, "Mouse5"}};

// Defaults match a 300x200 pad crossed in ~6000 mouse counts, with a trail ~60% of its size.
constexpr float kCountsToPad = 0.000204f;
constexpr float kTrailToPad = 0.612f;

std::string KeyName(UINT vk) {
  if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, char(vk));
  if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
  for (auto& [name, v] : kNamedKeys)
    if (v == vk) return name;
  return {};
}

// "w" -> "W", "lmb" -> "LMB"; "" if it isn't a key the overlay knows.
std::string Canonical(const std::string& s) {
  const std::string want = Lower(s);
  for (auto* m : kMouseNames)
    if (Lower(m) == want) return m;
  for (UINT vk = 1; vk < 256; vk++)
    if (auto n = KeyName(vk); !n.empty() && Lower(n) == want) return n;
  return {};
}

int64_t Ms(int64_t ms) { return ms * QpcFreq() / 1000; }

D2D1_POINT_2F CatmullRom(D2D1_POINT_2F p0, D2D1_POINT_2F p1, D2D1_POINT_2F p2, D2D1_POINT_2F p3, float t) {
  const float t2 = t * t, t3 = t2 * t;
  auto f = [&](float a, float b, float c, float d) {
    return 0.5f * (2 * b + (c - a) * t + (2 * a - 5 * b + 4 * c - d) * t2 + (3 * b - a - 3 * c + d) * t3);
  };
  return {f(p0.x, p1.x, p2.x, p3.x), f(p0.y, p1.y, p2.y, p3.y)};
}
}  // namespace

InputOverlay::InputOverlay(const OverlayConfig& cfg, DWORD game_pid) : cfg_(cfg), pid_(game_pid) {
  for (auto k : cfg_.keys) {
    const std::string name = Canonical(k.key);
    if (name.empty()) {
      Log("input overlay: unknown key \"{}\"", k.key);
      continue;
    }
    k.key = name;
    labels_.push_back(name == "Mouse" ? L"" : Wide(k.label.empty() ? name : k.label));
    width_ = std::max(width_, k.x + k.w);
    height_ = std::max(height_, k.y + k.h);
    if (name == "Mouse") pad_size_ = std::sqrt(k.w * k.h);
    keys_.push_back(k);
  }
  std::promise<void> started;
  auto ready = started.get_future();
  thread_ = std::thread(&InputOverlay::Loop, this, &started);
  ready.wait();
}

InputOverlay::~InputOverlay() {
  PostThreadMessageW(thread_id_, WM_QUIT, 0, 0);
  thread_.join();
}

void InputOverlay::Loop(std::promise<void>* started) {
  // A message-only window that gets Raw Input even while the game has focus (no hooks, nothing in the game).
  HINSTANCE inst = GetModuleHandleW(nullptr);
  WNDCLASSW wc{};
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = inst;
  wc.lpszClassName = L"ClipInput";
  RegisterClassW(&wc);  // fails harmlessly on the second overlay
  HWND w = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
  RAWINPUTDEVICE devs[] = {{0x01, 0x06, RIDEV_INPUTSINK, w}, {0x01, 0x02, RIDEV_INPUTSINK, w}};  // keyboard, mouse
  if (!w || !RegisterRawInputDevices(devs, 2, sizeof devs[0])) Log("input overlay: no input ({})", GetLastError());
  thread_id_ = GetCurrentThreadId();
  started->set_value();

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (msg.message == WM_INPUT) OnInput(HRAWINPUT(msg.lParam));
    DispatchMessageW(&msg);
  }
  for (auto& d : devs) d.dwFlags = RIDEV_REMOVE, d.hwndTarget = nullptr;
  RegisterRawInputDevices(devs, 2, sizeof devs[0]);
  if (w) DestroyWindow(w);
}

void InputOverlay::OnInput(HRAWINPUT h) {
  RAWINPUT ri;
  UINT size = sizeof ri;
  if (GetRawInputData(h, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) == UINT(-1)) return;

  DWORD fg = 0;
  GetWindowThreadProcessId(GetForegroundWindow(), &fg);
  if (fg != pid_) {  // not the game: show nothing, and let go of anything held
    std::lock_guard l(m_);
    down_.clear();
    lmb_ = false;
    return;
  }

  if (ri.header.dwType == RIM_TYPEKEYBOARD) {
    const RAWKEYBOARD& k = ri.data.keyboard;
    if (k.VKey != 0xFF)
      if (auto name = KeyName(k.VKey); !name.empty()) Set(name, !(k.Flags & RI_KEY_BREAK));
    return;
  }
  if (ri.header.dwType != RIM_TYPEMOUSE) return;
  const RAWMOUSE& m = ri.data.mouse;
  for (auto& b : kButtons) {
    if (m.usButtonFlags & b.down) Set(b.name, true);
    if (m.usButtonFlags & b.up) Set(b.name, false);
  }
  std::lock_guard l(m_);
  if (m.usButtonFlags & RI_MOUSE_WHEEL) lit_until_[SHORT(m.usButtonData) > 0 ? "WheelUp" : "WheelDown"] = QpcNow() + Ms(150);
  if (!(m.usFlags & MOUSE_MOVE_ABSOLUTE) && (m.lLastX || m.lLastY)) {
    const float k = kCountsToPad * cfg_.mouse_sensitivity * pad_size_;
    trail_.Move(m.lLastX * k, m.lLastY * k, QpcNow(), lmb_, Ms(4));
  }
}

void InputOverlay::Set(const std::string& key, bool down) {
  std::lock_guard l(m_);
  if (key == "LMB") lmb_ = down;
  if (down) down_.insert(key);
  else down_.erase(key);
}

void InputOverlay::Fill(ID2D1DeviceContext* dc, const D2D1_RECT_F& r, float u, bool on) {
  const float op = cfg_.opacity;
  const D2D1_ROUNDED_RECT rr{r, 0.15f * u, 0.15f * u};
  brush_->SetColor(on ? D2D1::ColorF(1, 1, 1, 0.9f * op) : D2D1::ColorF(0, 0, 0, 0.45f * op));
  dc->FillRoundedRectangle(rr, brush_.get());
  brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.3f * op));
  dc->DrawRoundedRectangle(rr, brush_.get(), 0.04f * u);
}

void InputOverlay::Draw(ID2D1DeviceContext* dc, D2D1_SIZE_F size) {
  const float u = size.height * 0.04f * cfg_.scale;  // one key width, ~43 px at 1080p
  if (dc != res_dc_ || u != res_u_) {
    brush_ = nullptr;
    round_ = nullptr;
    text_ = nullptr;
    winrt::check_hresult(dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), brush_.put()));
    winrt::com_ptr<ID2D1Factory> factory;
    dc->GetFactory(factory.put());
    const D2D1_STROKE_STYLE_PROPERTIES sp{D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                          D2D1_LINE_JOIN_ROUND};
    winrt::check_hresult(factory->CreateStrokeStyle(sp, nullptr, 0, round_.put()));
    winrt::com_ptr<IDWriteFactory> dw;
    winrt::check_hresult(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                             reinterpret_cast<IUnknown**>(dw.put())));
    winrt::check_hresult(dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                              DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 0.32f * u, L"",
                                              text_.put()));
    text_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    text_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    text_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    res_dc_ = dc;
    res_u_ = u;
  }

  const float margin = 0.6f * u, gap = 0.06f * u;
  const bool right = cfg_.corner.find("right") != std::string::npos;
  const bool bottom = cfg_.corner.find("bottom") != std::string::npos;
  const float ox = right ? size.width - margin - width_ * u : margin;
  const float oy = bottom ? size.height - margin - height_ * u : margin;
  const int64_t now = QpcNow();

  std::lock_guard l(m_);
  for (size_t i = 0; i < keys_.size(); i++) {
    const auto& k = keys_[i];
    const D2D1_RECT_F r{ox + k.x * u + gap, oy + k.y * u + gap, ox + (k.x + k.w) * u - gap, oy + (k.y + k.h) * u - gap};
    if (k.key == "Mouse") {
      DrawPad(dc, r, u, now);
      continue;
    }
    auto lit = lit_until_.find(k.key);
    const bool on = down_.contains(k.key) || (lit != lit_until_.end() && lit->second > now);
    Fill(dc, r, u, on);
    brush_->SetColor(on ? D2D1::ColorF(0, 0, 0, cfg_.opacity) : D2D1::ColorF(1, 1, 1, cfg_.opacity));
    dc->DrawTextW(labels_[i].c_str(), UINT32(labels_[i].size()), text_.get(), r, brush_.get());
  }
}

void InputOverlay::DrawPad(ID2D1DeviceContext* dc, D2D1_RECT_F r, float u, int64_t now) {
  const float op = cfg_.opacity;
  const int64_t max_age = Ms(cfg_.mouse_trail_ms);
  trail_.Trim(now, max_age, kTrailToPad * pad_size_ * cfg_.mouse_trail_length);
  Fill(dc, r, u, false);
  dc->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

  const float pw = r.right - r.left, ph = r.bottom - r.top, cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
  const bool pan = cfg_.mouse_mode == "pan";
  auto px = [&](const MouseTrail::Pt& p) -> D2D1_POINT_2F {
    if (pan) return {cx + (p.x - trail_.x()) * u, cy + (p.y - trail_.y()) * u};  // newest point stays centred
    return {r.left + WrapInPad(p.x * u, pw), r.top + WrapInPad(p.y * u, ph)};
  };
  auto jump = [&](D2D1_POINT_2F a, D2D1_POINT_2F b) {  // crossed a wrapped edge: don't join
    return !pan && (std::abs(b.x - a.x) > pw / 2 || std::abs(b.y - a.y) > ph / 2);
  };

  if (pan) {  // dot grid scrolling against the movement, so the pad reads as moving
    const float s = 0.5f * u, offx = std::fmod(-trail_.x() * u, s), offy = std::fmod(-trail_.y() * u, s);
    brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.15f * op));
    for (float y = r.top + offy - s; y < r.bottom + s; y += s)
      for (float x = r.left + offx - s; x < r.right + s; x += s)
        dc->FillEllipse(D2D1::Ellipse({x, y}, 0.03f * u, 0.03f * u), brush_.get());
  }

  const auto& pts = trail_.points();
  const float width = 0.06f * u;
  const int taper = 12;  // the oldest points narrow to nothing
  for (size_t i = 1; i < pts.size(); i++) {
    const D2D1_POINT_2F p1 = px(pts[i - 1]), p2 = px(pts[i]);
    if (jump(p1, p2)) continue;
    D2D1_POINT_2F p0 = i >= 2 ? px(pts[i - 2]) : p1, p3 = i + 1 < pts.size() ? px(pts[i + 1]) : p2;
    if (jump(p0, p1)) p0 = p1;
    if (jump(p2, p3)) p3 = p2;
    const bool m1 = pts[i].m1;
    const float fade = 1 - float(now - pts[i].t) / float(max_age);
    const float w = width * (m1 ? 1.5f : 1) * std::min(1.f, float(i) / taper);
    brush_->SetColor(m1 ? D2D1::ColorF(1, 0.8f, 0.25f, fade * op) : D2D1::ColorF(1, 1, 1, fade * op));
    D2D1_POINT_2F a = p1;
    for (int s = 1; s <= 3; s++) {  // smooth the corners between samples
      const D2D1_POINT_2F b = CatmullRom(p0, p1, p2, p3, s / 3.f);
      dc->DrawLine(a, b, brush_.get(), w, round_.get());
      a = b;
    }
  }
  if (!pts.empty()) {
    const auto& tip = pts.back();
    const float fade = 1 - float(now - tip.t) / float(max_age);
    brush_->SetColor(tip.m1 && lmb_ ? D2D1::ColorF(1, 0.8f, 0.25f, fade * op) : D2D1::ColorF(1, 1, 1, fade * op));
    const float rad = 1.2f * width * (tip.m1 && lmb_ ? 1.5f : 1);
    dc->FillEllipse(D2D1::Ellipse(px(tip), rad, rad), brush_.get());
  }
  dc->PopAxisAlignedClip();
}
