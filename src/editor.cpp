// The overlay editor: a live preview to drag and resize keys in, and the look settings beside it.
#include "overlay.h"

#include <commctrl.h>
#include <commdlg.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

namespace {
enum : int {
  IDC_PRESET = 100, IDC_APPLY_PRESET, IDC_KEY, IDC_ADD, IDC_LABEL, IDC_REMOVE, IDC_CORNER, IDC_MODE, IDC_FONT,
  IDC_SAVE, IDC_CANCEL,
  IDC_SLIDER = 200,  // + index into kSliders, then the trail fade slider
  IDC_COLOR = 300,   // + index into kColors
};
constexpr float kSnap = 0.25f;  // key widths
constexpr UINT_PTR kDemoTimer = 1;

const struct {
  const wchar_t* name;
  int lo, hi;  // slider positions = value x 100
  float OverlayConfig::*field;
} kSliders[] = {{L"Size", 25, 300, &OverlayConfig::scale},
                {L"Opacity", 0, 100, &OverlayConfig::opacity},
                {L"Roundness", 0, 50, &OverlayConfig::roundness},
                {L"Mouse sensitivity", 10, 500, &OverlayConfig::mouse_sensitivity},
                {L"Trail length", 0, 300, &OverlayConfig::mouse_trail_length},
                {L"Trail width", 25, 400, &OverlayConfig::trail_width}};
constexpr int kFadeSlider = int(std::size(kSliders));  // mouse_trail_ms
const struct {
  const wchar_t* name;
  std::string OverlayConfig::*field;
} kColors[] = {{L"Key", &OverlayConfig::idle_color},          {L"Pressed key", &OverlayConfig::pressed_color},
               {L"Text", &OverlayConfig::text_color},         {L"Pressed text", &OverlayConfig::pressed_text_color},
               {L"Border", &OverlayConfig::border_color},     {L"Trail", &OverlayConfig::trail_color},
               {L"Trail while clicking", &OverlayConfig::click_color}};
const char* const kCorners[] = {"top-left", "top-right", "bottom-left", "bottom-right"};

float Snap(float v) { return std::round(v / kSnap) * kSnap; }
bool Inside(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

struct Editor {
  HWND wnd = nullptr, canvas = nullptr;
  OverlayConfig cfg;
  std::function<void(const OverlayConfig&)> save;
  std::unique_ptr<OverlayPainter> painter;
  OverlayInput preview;  // keys pressed in the editor, plus a demo mouse trail
  int selected = -1;
  enum { kNone, kMove, kResize } drag = kNone;
  D2D1_POINT_2F drag_from{};
  OverlayKey drag_key;
  float u = 1;  // key width in the canvas, px
  D2D1_POINT_2F origin{};

  winrt::com_ptr<ID2D1Factory> factory;
  winrt::com_ptr<ID2D1HwndRenderTarget> rt;
  winrt::com_ptr<ID2D1SolidColorBrush> brush;

  struct Row {
    HWND label, a, b;
  };
  std::vector<Row> rows;
  HWND preset = nullptr, key = nullptr, label = nullptr, remove = nullptr, corner = nullptr, mode = nullptr,
       font_edit = nullptr, save_btn = nullptr, cancel_btn = nullptr;
  std::vector<HWND> sliders, slider_labels, colors;
  HFONT font = nullptr;
  bool filling = false;  // setting controls from cfg: ignore their change notifications
  UINT dpi = 96;
  int S(int v) const { return MulDiv(v, dpi, 96); }
};
Editor* ed = nullptr;

void Rebuild() {
  ed->painter = std::make_unique<OverlayPainter>(ed->cfg);
  InvalidateRect(ed->canvas, nullptr, FALSE);
}

// Controls show cfg.
void Fill() {
  ed->filling = true;
  const bool sel = ed->selected >= 0;
  SetWindowTextW(ed->label, sel ? Wide(ed->cfg.keys[ed->selected].label).c_str() : L"");
  EnableWindow(ed->label, sel && ed->cfg.keys[ed->selected].key != "Mouse");
  EnableWindow(ed->remove, sel);
  auto corner = std::find(std::begin(kCorners), std::end(kCorners), ed->cfg.corner);
  ComboBox_SetCurSel(ed->corner, corner == std::end(kCorners) ? 2 : int(corner - std::begin(kCorners)));
  ComboBox_SetCurSel(ed->mode, ed->cfg.mouse_mode == "pan" ? 1 : 0);
  for (int i = 0; i <= kFadeSlider; i++) {
    const int pos = i == kFadeSlider ? ed->cfg.mouse_trail_ms : int(std::lround(ed->cfg.*kSliders[i].field * 100));
    SendMessageW(ed->sliders[i], TBM_SETPOS, TRUE, pos);
    SetWindowTextW(ed->slider_labels[i], i == kFadeSlider ? std::format(L"Trail fade: {} ms", pos).c_str()
                                                          : std::format(L"{}: {}%", kSliders[i].name, pos).c_str());
  }
  for (size_t i = 0; i < std::size(kColors); i++) SetWindowTextW(ed->colors[i], Wide(ed->cfg.*kColors[i].field).c_str());
  SetWindowTextW(ed->font_edit, Wide(ed->cfg.font).c_str());
  ed->filling = false;
}

void Select(int i) {
  ed->selected = i;
  Fill();
  InvalidateRect(ed->canvas, nullptr, FALSE);
}

void RemoveSelected() {
  if (ed->selected < 0) return;
  ed->cfg.keys.erase(ed->cfg.keys.begin() + ed->selected);
  Select(-1);
  Rebuild();
}

// Moves the layout so its top-left key starts at 0,0.
void Normalize() {
  if (ed->cfg.keys.empty()) return;
  float x = 1e9f, y = 1e9f;
  for (auto& k : ed->cfg.keys) x = std::min(x, k.x), y = std::min(y, k.y);
  for (auto& k : ed->cfg.keys) k.x -= x, k.y -= y;
}

D2D1_RECT_F HandleRect(const D2D1_RECT_F& r) {
  const float h = float(ed->S(10));
  return {r.right - h, r.bottom - h, r.right + 3, r.bottom + 3};
}

void PickColor(size_t i) {
  static COLORREF custom[16] = {};
  std::string& field = ed->cfg.*kColors[i].field;
  D2D1_COLOR_F c = ParseColor(field, {1, 1, 1, 1});
  CHOOSECOLORW cc{sizeof cc};
  cc.hwndOwner = ed->wnd;
  cc.rgbResult = RGB(std::lround(c.r * 255), std::lround(c.g * 255), std::lround(c.b * 255));
  cc.lpCustColors = custom;
  cc.Flags = CC_RGBINIT | CC_FULLOPEN;
  if (!ChooseColorW(&cc)) return;
  c = {GetRValue(cc.rgbResult) / 255.f, GetGValue(cc.rgbResult) / 255.f, GetBValue(cc.rgbResult) / 255.f, c.a};
  field = ColorHex(c);  // keeps the colour's transparency; Opacity scales everything
  Fill();
  Rebuild();
}

void Paint() {
  RECT rc;
  GetClientRect(ed->canvas, &rc);
  if (!ed->rt) {
    const auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(), 96, 96);
    winrt::check_hresult(ed->factory->CreateHwndRenderTarget(
        props, D2D1::HwndRenderTargetProperties(ed->canvas, D2D1::SizeU(rc.right, rc.bottom)), ed->rt.put()));
    winrt::check_hresult(ed->rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), ed->brush.put()));
    ed->painter = std::make_unique<OverlayPainter>(ed->cfg);  // its resources belonged to the old target
  }
  auto dc = ed->rt.as<ID2D1DeviceContext>();
  const float W = float(rc.right), H = float(rc.bottom), m = float(ed->S(16)), edit_h = H * 0.64f;
  auto& p = *ed->painter;
  const int64_t now = QpcNow();

  ed->rt->BeginDraw();
  ed->rt->Clear(D2D1::ColorF(0.11f, 0.11f, 0.13f));
  const float c = float(ed->S(12));  // checkerboard, so see-through colours read as see-through
  ed->brush->SetColor(D2D1::ColorF(0.17f, 0.17f, 0.2f));
  for (int iy = 0; iy * c < edit_h; iy++)
    for (int ix = (iy & 1); ix * c < W; ix += 2)
      ed->rt->FillRectangle(D2D1::RectF(ix * c, iy * c, ix * c + c, std::min(iy * c + c, edit_h)), ed->brush.get());

  if (p.width() > 0) {
    if (ed->drag == Editor::kNone) {  // hold the zoom still while dragging
      ed->u = std::min({(W - 2 * m) / p.width(), (edit_h - 2 * m) / p.height(), float(ed->S(72))});
      ed->origin = {(W - p.width() * ed->u) / 2, (edit_h - p.height() * ed->u) / 2};
    }
    p.Draw(dc.get(), ed->origin, ed->u, ed->preview, now);
    if (ed->selected >= 0) {
      const auto r = p.KeyRect(ed->selected, ed->origin, ed->u);
      ed->brush->SetColor(D2D1::ColorF(0.3f, 0.65f, 1));
      ed->rt->DrawRectangle(D2D1::RectF(r.left - 3, r.top - 3, r.right + 3, r.bottom + 3), ed->brush.get(), 2);
      ed->rt->FillRectangle(HandleRect(r), ed->brush.get());
    }
  }

  // Below: a 16:9 frame showing where the overlay lands in a clip.
  float fh = H - edit_h - 2 * m, fw = fh * 16 / 9;
  if (fw > W - 2 * m) fw = W - 2 * m, fh = fw * 9 / 16;
  const D2D1_RECT_F frame{(W - fw) / 2, edit_h + m, (W + fw) / 2, edit_h + m + fh};
  ed->brush->SetColor(D2D1::ColorF(0.22f, 0.3f, 0.36f));
  ed->rt->FillRectangle(frame, ed->brush.get());
  if (p.width() > 0) {
    float u;
    D2D1_POINT_2F o;
    p.Place({fw, fh}, u, o);
    ed->rt->PushAxisAlignedClip(frame, D2D1_ANTIALIAS_MODE_ALIASED);
    p.Draw(dc.get(), {o.x + frame.left, o.y + frame.top}, u, ed->preview, now);
    ed->rt->PopAxisAlignedClip();
  }
  if (ed->rt->EndDraw() == D2DERR_RECREATE_TARGET) {
    ed->rt = nullptr;
    ed->brush = nullptr;
  }
}

// A wandering mouse with a click every few seconds, so trail settings show while you change them.
void Demo() {
  const int64_t now = QpcNow();
  const double t = double(now) / double(QpcFreq());
  const float k = kMouseCountsToPad * ed->cfg.mouse_sensitivity * ed->painter->pad_size();
  const float dx = float(std::cos(t * 1.7) * 90 + std::cos(t * 4.1) * 40);
  const float dy = float(std::sin(t * 2.3) * 70 + std::sin(t * 5.3) * 30);
  const bool click = std::fmod(t, 3.0) < 0.5;
  if (click != ed->preview.lmb) {
    ed->preview.lmb = click;
    if (click) ed->preview.down.insert("LMB");
    else ed->preview.down.erase("LMB");
  }
  if (std::fmod(t, 4.0) < 0.02) ed->preview.lit_until["WheelUp"] = now + QpcFreq() / 6;
  ed->preview.trail.Move(dx * k, dy * k, now, click, QpcFreq() / 250);
  InvalidateRect(ed->canvas, nullptr, FALSE);
}

LRESULT CALLBACK CanvasProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
  if (!ed) return DefWindowProcW(w, msg, wp, lp);
  const float x = float(GET_X_LPARAM(lp)), y = float(GET_Y_LPARAM(lp));
  auto units = [&] { return D2D1_POINT_2F{(x - ed->origin.x) / ed->u, (y - ed->origin.y) / ed->u}; };
  switch (msg) {
    case WM_PAINT:
      try {  // nothing may throw out of a window procedure
        Paint();
      } catch (const winrt::hresult_error& e) {
        static bool logged = false;
        if (!std::exchange(logged, true)) Log("overlay editor: can't draw: {}", winrt::to_string(e.message()));
        ed->rt = nullptr;  // start over with a new target next time
        ed->brush = nullptr;
      }
      ValidateRect(w, nullptr);
      return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE:
      if (ed->rt) ed->rt->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
      return 0;
    case WM_TIMER: Demo(); return 0;
    case WM_LBUTTONDOWN: {
      SetFocus(w);
      SetCapture(w);
      auto& p = *ed->painter;
      ed->drag = Editor::kNone;
      if (ed->selected >= 0 && Inside(HandleRect(p.KeyRect(ed->selected, ed->origin, ed->u)), x, y)) {
        ed->drag = Editor::kResize;
      } else {
        int hit = -1;
        for (int i = int(ed->cfg.keys.size()) - 1; i >= 0 && hit < 0; i--)
          if (Inside(p.KeyRect(i, ed->origin, ed->u), x, y)) hit = i;
        Select(hit);
        if (hit >= 0) ed->drag = Editor::kMove;
      }
      if (ed->drag != Editor::kNone) {
        ed->drag_from = units();
        ed->drag_key = ed->cfg.keys[ed->selected];
      }
      return 0;
    }
    case WM_MOUSEMOVE:
      if (ed->drag != Editor::kNone && (wp & MK_LBUTTON)) {
        const auto p = units();
        const float dx = p.x - ed->drag_from.x, dy = p.y - ed->drag_from.y;
        auto& k = ed->cfg.keys[ed->selected];
        if (ed->drag == Editor::kMove) {
          k.x = Snap(ed->drag_key.x + dx);
          k.y = Snap(ed->drag_key.y + dy);
        } else {
          k.w = std::max(0.5f, Snap(ed->drag_key.w + dx));
          k.h = std::max(0.5f, Snap(ed->drag_key.h + dy));
        }
        Rebuild();
      }
      return 0;
    case WM_LBUTTONUP:
      ReleaseCapture();
      if (ed->drag != Editor::kNone) {
        ed->drag = Editor::kNone;
        Normalize();
        Rebuild();
      }
      return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
      if (msg == WM_SYSKEYDOWN && wp == VK_F4) break;  // Alt+F4 still closes
      if (wp == VK_DELETE || wp == VK_BACK) {
        if (ed->selected >= 0) return RemoveSelected(), 0;
      } else if (ed->selected >= 0 && (wp == VK_LEFT || wp == VK_RIGHT || wp == VK_UP || wp == VK_DOWN)) {
        auto& k = ed->cfg.keys[ed->selected];  // nudge
        k.x += wp == VK_LEFT ? -kSnap : wp == VK_RIGHT ? kSnap : 0;
        k.y += wp == VK_UP ? -kSnap : wp == VK_DOWN ? kSnap : 0;
        Normalize();
        Rebuild();
        return 0;
      } else if (wp == VK_ESCAPE) {
        return Select(-1), 0;
      }
      if (auto name = OverlayKeyName(UINT(wp)); !name.empty()) ed->preview.down.insert(name);  // try the keys out
      return 0;
    case WM_KEYUP:
    case WM_SYSKEYUP:
      if (auto name = OverlayKeyName(UINT(wp)); !name.empty()) ed->preview.down.erase(name);
      return 0;
    case WM_SYSCHAR: return 0;
    case WM_KILLFOCUS: ed->preview.down.clear(); return 0;
  }
  return DefWindowProcW(w, msg, wp, lp);
}

void SetFonts() {
  if (ed->font) DeleteObject(ed->font);
  NONCLIENTMETRICSW m{sizeof m};
  SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof m, &m, 0, ed->dpi);
  ed->font = CreateFontIndirectW(&m.lfMessageFont);
  EnumChildWindows(
      ed->wnd, [](HWND c, LPARAM f) { return SendMessageW(c, WM_SETFONT, WPARAM(f), TRUE), TRUE; },
      LPARAM(ed->font));
}

void Layout() {
  if (!ed || !ed->canvas) return;
  RECT rc;
  GetClientRect(ed->wnd, &rc);
  const int panel = ed->S(340), m = ed->S(10), row = ed->S(28), lw = ed->S(140), bw = ed->S(66), h = ed->S(24);
  const int x = rc.right - panel + m, w = panel - 2 * m;
  MoveWindow(ed->canvas, 0, 0, rc.right - panel, rc.bottom, TRUE);
  int y = m;
  for (auto& r : ed->rows) {
    wchar_t cls[32];
    GetClassNameW(r.a, cls, 32);
    const int aw = w - lw - (r.b ? bw + ed->S(6) : 0);
    MoveWindow(r.label, x, y, lw, h, TRUE);
    MoveWindow(r.a, x + lw, y, aw, _wcsicmp(cls, WC_COMBOBOXW) ? h : ed->S(320), TRUE);  // combo height = its list
    if (r.b) MoveWindow(r.b, x + lw + aw + ed->S(6), y, bw, h, TRUE);
    y += row;
  }
  MoveWindow(ed->save_btn, x + w - 2 * bw - ed->S(6), rc.bottom - m - ed->S(28), bw, ed->S(28), TRUE);
  MoveWindow(ed->cancel_btn, x + w - bw, rc.bottom - m - ed->S(28), bw, ed->S(28), TRUE);
}

void OnCommand(int id, int code) {
  const auto text = [](HWND c) {
    std::wstring s(GetWindowTextLengthW(c), L'\0');
    GetWindowTextW(c, s.data(), int(s.size() + 1));
    return Utf8(s);
  };
  switch (id) {
    case IDC_APPLY_PRESET:
      if (int i = ComboBox_GetCurSel(ed->preset); i >= 0) {
        ed->cfg.keys = OverlayPresets()[i].keys;
        Select(-1);
        Rebuild();
      }
      return;
    case IDC_ADD:
      if (int i = ComboBox_GetCurSel(ed->key); i >= 0) {
        const std::string name = OverlayKeyNames()[i];
        const bool wide = name == "Shift" || name == "Ctrl" || name == "Tab" || name == "Caps" || name == "Enter" ||
                          name == "Backspace";
        const float w = name == "Mouse" ? 2.f : name == "Space" ? 3.f : wide ? 1.5f : 1.f;
        ed->cfg.keys.push_back({name, "", 0, ed->painter->height(), w, name == "Mouse" ? 2.f : 1.f});
        Select(int(ed->cfg.keys.size() - 1));
        Rebuild();
      }
      return;
    case IDC_REMOVE: return RemoveSelected();
    case IDC_LABEL:
      if (code == EN_CHANGE && !ed->filling && ed->selected >= 0) {
        ed->cfg.keys[ed->selected].label = text(ed->label);
        Rebuild();
      }
      return;
    case IDC_FONT:
      if (code == EN_CHANGE && !ed->filling) {
        ed->cfg.font = text(ed->font_edit);
        Rebuild();
      }
      return;
    case IDC_CORNER:
      if (code == CBN_SELCHANGE) ed->cfg.corner = kCorners[std::max(0, ComboBox_GetCurSel(ed->corner))];
      return Rebuild();
    case IDC_MODE:
      if (code == CBN_SELCHANGE) ed->cfg.mouse_mode = ComboBox_GetCurSel(ed->mode) == 1 ? "pan" : "wrap";
      return Rebuild();
    case IDC_SAVE: {
      auto save = std::move(ed->save);
      const OverlayConfig cfg = ed->cfg;
      DestroyWindow(ed->wnd);
      save(cfg);
      return;
    }
    case IDC_CANCEL: DestroyWindow(ed->wnd); return;
  }
  if (id >= IDC_COLOR && id < IDC_COLOR + int(std::size(kColors)) && code == BN_CLICKED) PickColor(id - IDC_COLOR);
}

LRESULT CALLBACK EditorProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
  if (!ed) return DefWindowProcW(w, msg, wp, lp);
  switch (msg) {
    case WM_SIZE: Layout(); return 0;
    case WM_GETMINMAXINFO:
      reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {ed->S(760), ed->S(640)};
      return 0;
    case WM_DPICHANGED: {
      ed->dpi = HIWORD(wp);
      const RECT* r = reinterpret_cast<RECT*>(lp);
      SetWindowPos(w, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      SetFonts();
      Layout();
      return 0;
    }
    case WM_HSCROLL:
      for (int i = 0; i <= kFadeSlider; i++)
        if (HWND(lp) == ed->sliders[i]) {
          const int pos = int(SendMessageW(ed->sliders[i], TBM_GETPOS, 0, 0));
          if (i == kFadeSlider) ed->cfg.mouse_trail_ms = pos;
          else ed->cfg.*kSliders[i].field = pos / 100.f;
          Fill();
          Rebuild();
        }
      return 0;
    case WM_COMMAND: OnCommand(LOWORD(wp), HIWORD(wp)); return 0;
    case WM_CLOSE: DestroyWindow(w); return 0;
    case WM_NCDESTROY:  // after the children are gone
      if (ed->font) DeleteObject(ed->font);
      delete ed;
      ed = nullptr;
      return 0;
  }
  return DefWindowProcW(w, msg, wp, lp);
}

HWND Child(const wchar_t* cls, const wchar_t* text, DWORD style, int id = 0) {
  return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, ed->wnd, HMENU(INT_PTR(id)),
                         GetModuleHandleW(nullptr), nullptr);
}

HWND Combo(int id, const std::vector<std::wstring>& items) {
  HWND c = Child(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, id);
  for (auto& s : items) ComboBox_AddString(c, s.c_str());
  ComboBox_SetCurSel(c, 0);
  return c;
}

void Row(const wchar_t* label, HWND a, HWND b = nullptr) {
  ed->rows.push_back({Child(WC_STATICW, label, SS_LEFT | SS_CENTERIMAGE), a, b});
}
}  // namespace

void ShowOverlayEditor(const OverlayConfig& cfg, std::function<void(const OverlayConfig&)> save) {
  if (ed) return (void)SetForegroundWindow(ed->wnd);
  static const bool registered = [] {
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpfnWndProc = EditorProc;
    wc.lpszClassName = L"ClipOverlayEditor";
    wc.hbrBackground = HBRUSH(COLOR_BTNFACE + 1);
    RegisterClassW(&wc);
    wc.lpfnWndProc = CanvasProc;
    wc.lpszClassName = L"ClipOverlayCanvas";
    wc.hbrBackground = nullptr;
    RegisterClassW(&wc);
    return true;
  }();
  (void)registered;

  winrt::com_ptr<ID2D1Factory> factory;
  if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr, factory.put_void())))
    return Log("overlay editor: no Direct2D");
  ed = new Editor;
  ed->cfg = cfg;
  std::erase_if(ed->cfg.keys, [](OverlayKey& k) { return (k.key = CanonicalKey(k.key)).empty(); });
  ed->save = std::move(save);
  ed->factory = std::move(factory);
  ed->painter = std::make_unique<OverlayPainter>(ed->cfg);

  ed->wnd = CreateWindowExW(0, L"ClipOverlayEditor", L"Clip input overlay", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                            CW_USEDEFAULT, 100, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  ed->dpi = GetDpiForWindow(ed->wnd);
  ed->canvas = CreateWindowExW(0, L"ClipOverlayCanvas", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, ed->wnd,
                               nullptr, GetModuleHandleW(nullptr), nullptr);

  std::vector<std::wstring> items;
  for (auto& p : OverlayPresets()) items.push_back(Wide(p.name));
  ed->preset = Combo(IDC_PRESET, items);
  Row(L"Layout", ed->preset, Child(WC_BUTTONW, L"Use", BS_PUSHBUTTON | WS_TABSTOP, IDC_APPLY_PRESET));
  items.clear();
  for (auto& n : OverlayKeyNames()) items.push_back(Wide(n == "Mouse" ? "Mouse (movement pad)" : n));
  ed->key = Combo(IDC_KEY, items);
  Row(L"Add a key", ed->key, Child(WC_BUTTONW, L"Add", BS_PUSHBUTTON | WS_TABSTOP, IDC_ADD));
  ed->label = Child(WC_EDITW, L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, IDC_LABEL);
  ed->remove = Child(WC_BUTTONW, L"Remove", BS_PUSHBUTTON | WS_TABSTOP, IDC_REMOVE);
  Row(L"Selected key's text", ed->label, ed->remove);
  ed->corner = Combo(IDC_CORNER, {L"Top left", L"Top right", L"Bottom left", L"Bottom right"});
  Row(L"Corner of the clip", ed->corner);
  ed->mode = Combo(IDC_MODE, {L"Dot wraps at the edges", L"Dot stays centred, pad scrolls"});
  Row(L"Mouse pad", ed->mode);
  for (int i = 0; i <= kFadeSlider; i++) {
    HWND tb = Child(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, IDC_SLIDER + i);
    SendMessageW(tb, TBM_SETRANGEMIN, FALSE, i == kFadeSlider ? 100 : kSliders[i].lo);
    SendMessageW(tb, TBM_SETRANGEMAX, TRUE, i == kFadeSlider ? 3000 : kSliders[i].hi);
    ed->sliders.push_back(tb);
    Row(L"", tb);
    ed->slider_labels.push_back(ed->rows.back().label);
  }
  for (size_t i = 0; i < std::size(kColors); i++) {
    ed->colors.push_back(Child(WC_BUTTONW, L"", BS_PUSHBUTTON | WS_TABSTOP, IDC_COLOR + int(i)));
    Row(kColors[i].name, ed->colors.back());
  }
  ed->font_edit = Child(WC_EDITW, L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, IDC_FONT);
  Row(L"Font", ed->font_edit);
  ed->save_btn = Child(WC_BUTTONW, L"Save", BS_DEFPUSHBUTTON | WS_TABSTOP, IDC_SAVE);
  ed->cancel_btn = Child(WC_BUTTONW, L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, IDC_CANCEL);

  SetFonts();
  Fill();
  RECT r{0, 0, ed->S(1000), ed->S(28) * int(ed->rows.size()) + ed->S(70)};
  AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, ed->dpi);
  RECT wa;
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
  const int ww = r.right - r.left, wh = std::min<int>(r.bottom - r.top, wa.bottom - wa.top);
  SetWindowPos(ed->wnd, nullptr, (wa.left + wa.right - ww) / 2, (wa.top + wa.bottom - wh) / 2, ww, wh, SWP_NOZORDER);
  Layout();
  ShowWindow(ed->wnd, SW_SHOW);
  SetForegroundWindow(ed->wnd);
  SetTimer(ed->canvas, kDemoTimer, 16, nullptr);
}
