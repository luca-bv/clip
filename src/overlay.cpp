#include "overlay.h"

#include <algorithm>

namespace {
const std::pair<const char*, UINT> kNamedKeys[] = {
    {"Space", VK_SPACE}, {"Shift", VK_SHIFT},    {"Ctrl", VK_CONTROL},  {"Alt", VK_MENU},      {"Tab", VK_TAB},
    {"Enter", VK_RETURN}, {"Esc", VK_ESCAPE},    {"Caps", VK_CAPITAL},  {"Backspace", VK_BACK}, {"Up", VK_UP},
    {"Down", VK_DOWN},   {"Left", VK_LEFT},      {"Right", VK_RIGHT},   {"Insert", VK_INSERT}, {"Home", VK_HOME},
    {"End", VK_END},     {"PageUp", VK_PRIOR},   {"PageDown", VK_NEXT}};
const char* const kMouseNames[] = {"Mouse", "LMB", "RMB", "MMB", "Mouse4", "Mouse5", "WheelUp", "WheelDown"};
const struct {
  USHORT down, up;
  const char* name;
} kButtons[] = {{RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP, "LMB"},
                {RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP, "RMB"},
                {RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, "MMB"},
                {RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_4_UP, "Mouse4"},
                {RI_MOUSE_BUTTON_5_DOWN, RI_MOUSE_BUTTON_5_UP, "Mouse5"}};

int64_t Ms(int64_t ms) { return ms * QpcFreq() / 1000; }

D2D1_POINT_2F CatmullRom(D2D1_POINT_2F p0, D2D1_POINT_2F p1, D2D1_POINT_2F p2, D2D1_POINT_2F p3, float t) {
  const float t2 = t * t, t3 = t2 * t;
  auto f = [&](float a, float b, float c, float d) {
    return 0.5f * (2 * b + (c - a) * t + (2 * a - 5 * b + 4 * c - d) * t2 + (3 * b - a - 3 * c + d) * t3);
  };
  return {f(p0.x, p1.x, p2.x, p3.x), f(p0.y, p1.y, p2.y, p3.y)};
}
}  // namespace

const std::vector<std::string>& OverlayKeyNames() {
  static const std::vector<std::string> names = [] {
    std::vector<std::string> v(std::begin(kMouseNames), std::end(kMouseNames));
    for (auto& [name, vk] : kNamedKeys) v.push_back(name);
    for (char c = 'A'; c <= 'Z'; c++) v.emplace_back(1, c);
    for (char c = '0'; c <= '9'; c++) v.emplace_back(1, c);
    for (int f = 1; f <= 24; f++) v.push_back("F" + std::to_string(f));
    return v;
  }();
  return names;
}

std::string OverlayKeyName(UINT vk) {
  if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, char(vk));
  if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
  for (auto& [name, v] : kNamedKeys)
    if (v == vk) return name;
  return {};
}

std::string CanonicalKey(const std::string& s) {
  const std::string want = Lower(s);
  for (auto& n : OverlayKeyNames())
    if (Lower(n) == want) return n;
  return {};
}

D2D1_COLOR_F ParseColor(const std::string& hex, D2D1_COLOR_F fallback) {
  if ((hex.size() != 7 && hex.size() != 9) || hex[0] != '#') return fallback;
  uint32_t v = 0;
  for (size_t i = 1; i < hex.size(); i++) {
    const char c = hex[i];
    const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    if (d < 0) return fallback;
    v = v << 4 | uint32_t(d);
  }
  if (hex.size() == 7) v = v << 8 | 0xFF;
  return {(v >> 24 & 0xFF) / 255.f, (v >> 16 & 0xFF) / 255.f, (v >> 8 & 0xFF) / 255.f, (v & 0xFF) / 255.f};
}

std::string ColorHex(D2D1_COLOR_F c) {
  auto b = [](float f) { return int(std::lround(std::clamp(f, 0.f, 1.f) * 255)); };
  return c.a >= 1 ? std::format("#{:02X}{:02X}{:02X}", b(c.r), b(c.g), b(c.b))
                  : std::format("#{:02X}{:02X}{:02X}{:02X}", b(c.r), b(c.g), b(c.b), b(c.a));
}

// ---- painter ----

OverlayPainter::OverlayPainter(const OverlayConfig& cfg) : cfg_(cfg) {
  cfg_.keys.clear();
  for (auto k : cfg.keys) {
    k.key = CanonicalKey(k.key);
    if (k.key.empty()) continue;
    labels_.push_back(k.key == "Mouse" ? L"" : Wide(k.label.empty() ? k.key : k.label));
    width_ = std::max(width_, k.x + k.w);
    height_ = std::max(height_, k.y + k.h);
    if (k.key == "Mouse") pad_size_ = std::sqrt(k.w * k.h);
    cfg_.keys.push_back(k);
  }
  const OverlayConfig def;
  auto color = [&](const std::string& s, const std::string& d) { return ParseColor(s, ParseColor(d, {1, 1, 1, 1})); };
  idle_ = color(cfg.idle_color, def.idle_color);
  pressed_ = color(cfg.pressed_color, def.pressed_color);
  text_ = color(cfg.text_color, def.text_color);
  pressed_text_ = color(cfg.pressed_text_color, def.pressed_text_color);
  border_ = color(cfg.border_color, def.border_color);
  trail_ = color(cfg.trail_color, def.trail_color);
  click_ = color(cfg.click_color, def.click_color);
}

void OverlayPainter::Place(D2D1_SIZE_F size, float& u, D2D1_POINT_2F& origin) const {
  u = size.height * 0.04f * cfg_.scale;  // one key width, ~43 px at 1080p
  const float margin = 0.6f * u;
  const bool right = cfg_.corner.find("right") != std::string::npos;
  const bool bottom = cfg_.corner.find("bottom") != std::string::npos;
  origin = {right ? size.width - margin - width_ * u : margin, bottom ? size.height - margin - height_ * u : margin};
}

D2D1_RECT_F OverlayPainter::KeyRect(size_t i, D2D1_POINT_2F o, float u) const {
  const auto& k = cfg_.keys[i];
  const float gap = 0.06f * u;
  return {o.x + k.x * u + gap, o.y + k.y * u + gap, o.x + (k.x + k.w) * u - gap, o.y + (k.y + k.h) * u - gap};
}

D2D1_COLOR_F OverlayPainter::Faded(D2D1_COLOR_F c, float alpha) const {
  c.a *= cfg_.opacity * std::clamp(alpha, 0.f, 1.f);
  return c;
}

void OverlayPainter::Fill(ID2D1DeviceContext* dc, const D2D1_RECT_F& r, float u, bool on) {
  const float rad = std::clamp(cfg_.roundness, 0.f, 0.5f) * u;
  const D2D1_ROUNDED_RECT rr{r, rad, rad};
  brush_->SetColor(Faded(on ? pressed_ : idle_));
  dc->FillRoundedRectangle(rr, brush_.get());
  brush_->SetColor(Faded(border_));
  dc->DrawRoundedRectangle(rr, brush_.get(), 0.04f * u);
}

void OverlayPainter::Draw(ID2D1DeviceContext* dc, D2D1_POINT_2F origin, float u, OverlayInput& in, int64_t now) {
  if (dc != res_dc_ || u != res_u_) {
    brush_ = nullptr;
    round_ = nullptr;
    text_format_ = nullptr;
    winrt::check_hresult(dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), brush_.put()));
    winrt::com_ptr<ID2D1Factory> factory;
    dc->GetFactory(factory.put());
    const D2D1_STROKE_STYLE_PROPERTIES sp{D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                          D2D1_LINE_JOIN_ROUND};
    winrt::check_hresult(factory->CreateStrokeStyle(sp, nullptr, 0, round_.put()));
    winrt::com_ptr<IDWriteFactory> dw;
    winrt::check_hresult(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                             reinterpret_cast<IUnknown**>(dw.put())));
    winrt::check_hresult(dw->CreateTextFormat(Wide(cfg_.font).c_str(), nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                              DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                              std::max(0.32f * u, 1.f), L"", text_format_.put()));
    text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    text_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    res_dc_ = dc;
    res_u_ = u;
  }
  for (size_t i = 0; i < cfg_.keys.size(); i++) {
    const auto& k = cfg_.keys[i];
    const D2D1_RECT_F r = KeyRect(i, origin, u);
    if (k.key == "Mouse") {
      DrawPad(dc, r, u, in, now);
      continue;
    }
    auto lit = in.lit_until.find(k.key);
    const bool on = in.down.contains(k.key) || (lit != in.lit_until.end() && lit->second > now);
    Fill(dc, r, u, on);
    brush_->SetColor(Faded(on ? pressed_text_ : text_));
    dc->DrawTextW(labels_[i].c_str(), UINT32(labels_[i].size()), text_format_.get(), r, brush_.get());
  }
}

void OverlayPainter::DrawPad(ID2D1DeviceContext* dc, D2D1_RECT_F r, float u, OverlayInput& in, int64_t now) {
  const int64_t max_age = Ms(std::max(cfg_.mouse_trail_ms, 1));
  auto& trail = in.trail;
  trail.Trim(now, max_age, kTrailToPad * pad_size_ * cfg_.mouse_trail_length);
  Fill(dc, r, u, false);
  dc->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

  const float pw = r.right - r.left, ph = r.bottom - r.top, cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
  const bool pan = cfg_.mouse_mode == "pan";
  auto px = [&](const MouseTrail::Pt& p) -> D2D1_POINT_2F {
    if (pan) return {cx + (p.x - trail.x()) * u, cy + (p.y - trail.y()) * u};  // newest point stays centred
    return {r.left + WrapInPad(p.x * u, pw), r.top + WrapInPad(p.y * u, ph)};
  };
  auto jump = [&](D2D1_POINT_2F a, D2D1_POINT_2F b) {  // crossed a wrapped edge: don't join
    return !pan && (std::abs(b.x - a.x) > pw / 2 || std::abs(b.y - a.y) > ph / 2);
  };

  if (pan) {  // dot grid scrolling against the movement, so the pad reads as moving
    const float s = 0.5f * u, offx = std::fmod(-trail.x() * u, s), offy = std::fmod(-trail.y() * u, s);
    brush_->SetColor(Faded(border_, 0.5f));
    for (float y = r.top + offy - s; y < r.bottom + s; y += s)
      for (float x = r.left + offx - s; x < r.right + s; x += s)
        dc->FillEllipse(D2D1::Ellipse({x, y}, 0.03f * u, 0.03f * u), brush_.get());
  }

  const auto& pts = trail.points();
  const float width = 0.06f * u * cfg_.trail_width;
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
    brush_->SetColor(Faded(m1 ? click_ : trail_, fade));
    D2D1_POINT_2F a = p1;
    for (int s = 1; s <= 3; s++) {  // smooth the corners between samples
      const D2D1_POINT_2F b = CatmullRom(p0, p1, p2, p3, s / 3.f);
      dc->DrawLine(a, b, brush_.get(), w, round_.get());
      a = b;
    }
  }
  if (!pts.empty()) {
    const auto& tip = pts.back();
    const bool m1 = tip.m1 && in.lmb;
    brush_->SetColor(Faded(m1 ? click_ : trail_, 1 - float(now - tip.t) / float(max_age)));
    const float rad = 1.2f * width * (m1 ? 1.5f : 1);
    dc->FillEllipse(D2D1::Ellipse(px(tip), rad, rad), brush_.get());
  }
  dc->PopAxisAlignedClip();
}

// ---- input ----

InputOverlay::InputOverlay(const OverlayConfig& cfg, DWORD game_pid)
    : cfg_(cfg), pid_(game_pid), painter_(cfg), listen_id_(raw_input::Listen([this](auto& ri) { OnInput(ri); })) {}

InputOverlay::~InputOverlay() { raw_input::Unlisten(listen_id_); }

void InputOverlay::Draw(ID2D1DeviceContext* dc, D2D1_SIZE_F frame) {
  float u;
  D2D1_POINT_2F origin;
  painter_.Place(frame, u, origin);
  std::lock_guard l(m_);
  painter_.Draw(dc, origin, u, in_, QpcNow());
}

void InputOverlay::OnInput(const RAWINPUT& ri) {
  DWORD fg = 0;
  GetWindowThreadProcessId(GetForegroundWindow(), &fg);
  std::lock_guard l(m_);
  if (fg != pid_) {  // not the game: show nothing, and let go of anything held
    in_.down.clear();
    in_.lmb = false;
    return;
  }
  auto set = [&](const std::string& key, bool down) {
    if (key == "LMB") in_.lmb = down;
    if (down) in_.down.insert(key);
    else in_.down.erase(key);
  };

  if (ri.header.dwType == RIM_TYPEKEYBOARD) {
    const RAWKEYBOARD& k = ri.data.keyboard;
    if (k.VKey != 0xFF)
      if (auto name = OverlayKeyName(k.VKey); !name.empty()) set(name, !(k.Flags & RI_KEY_BREAK));
    return;
  }
  if (ri.header.dwType != RIM_TYPEMOUSE) return;
  const RAWMOUSE& m = ri.data.mouse;
  for (auto& b : kButtons) {
    if (m.usButtonFlags & b.down) set(b.name, true);
    if (m.usButtonFlags & b.up) set(b.name, false);
  }
  if (m.usButtonFlags & RI_MOUSE_WHEEL) in_.lit_until[SHORT(m.usButtonData) > 0 ? "WheelUp" : "WheelDown"] = QpcNow() + Ms(150);
  if (!(m.usFlags & MOUSE_MOVE_ABSOLUTE) && (m.lLastX || m.lLastY)) {
    const float k = kMouseCountsToPad * cfg_.mouse_sensitivity * painter_.pad_size();
    in_.trail.Move(m.lLastX * k, m.lLastY * k, QpcNow(), in_.lmb, Ms(4));
  }
}
