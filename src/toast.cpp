#include "toast.h"

#include <unknwn.h>

#include <d2d1.h>
#include <dwrite.h>
#include <winrt/base.h>

#include <algorithm>
#include <cmath>
#include <fstream>

namespace {
constexpr UINT_PTR kTick = 1;
constexpr double kIn = 220, kHold = 2400, kWait = 10000, kOut = 260;  // ms
constexpr float kW = 300, kH = 60, kMargin = 20;                       // dips

struct {
  HWND wnd = nullptr;
  winrt::com_ptr<ID2D1Factory> d2d;
  winrt::com_ptr<IDWriteFactory> dw;
  float dpi = 96;
  int w = 0, h = 0;
  POINT at{};  // resting top-left
  ULONGLONG since = 0;
  double hold = kHold;
  std::vector<uint8_t>* capture = nullptr;  // clip_bench: a copy of the painted pixels
} t;

float Px(float dip) { return dip * t.dpi / 96; }
double EaseOut(double x) { return 1 - std::pow(1 - std::clamp(x, 0.0, 1.0), 3); }

// Slides in from the screen's right edge, waits, slides back out.
void Place(double ms) {
  const double in = EaseOut(ms / kIn), out = EaseOut((ms - kIn - t.hold) / kOut);
  const double off = (1 - in) + out;  // 0 = resting, 1 = just off the edge
  BLENDFUNCTION blend{AC_SRC_OVER, 0, BYTE(std::lround(255 * std::clamp(1 - out * 0.6, 0.0, 1.0))), AC_SRC_ALPHA};
  POINT p{t.at.x + LONG(std::lround((t.w + Px(kMargin)) * off)), t.at.y};
  UpdateLayeredWindow(t.wnd, nullptr, &p, nullptr, nullptr, nullptr, 0, &blend, ULW_ALPHA);
}

LRESULT CALLBACK Proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_TIMER && wp == kTick) {
    const double ms = double(GetTickCount64() - t.since);
    if (ms >= kIn + t.hold + kOut) HideToast();
    else Place(ms);
    return 0;
  }
  if (msg == WM_NCHITTEST) return HTTRANSPARENT;
  if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
  return DefWindowProcW(w, msg, wp, lp);
}

bool Ready() {
  if (t.wnd) return true;
  try {
    winrt::check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, t.d2d.put()));
    winrt::check_hresult(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                             reinterpret_cast<IUnknown**>(t.dw.put())));
  } catch (const winrt::hresult_error& e) {
    Log("toast: {}", winrt::to_string(e.message()));
    return false;
  }
  WNDCLASSW wc{};
  wc.lpfnWndProc = Proc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"ClipToast";
  RegisterClassW(&wc);
  t.wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                          wc.lpszClassName, L"Clip", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
  if (!t.wnd) return false;
  SetWindowDisplayAffinity(t.wnd, WDA_EXCLUDEFROMCAPTURE);
  return true;
}

// Draws the card into a premultiplied 32-bit bitmap and hands it to the layered window.
void Paint(const std::wstring& title, const std::wstring& detail, bool ok, BYTE alpha) {
  const float W = Px(kW), H = Px(kH);
  t.w = int(std::ceil(W)), t.h = int(std::ceil(H));
  BITMAPINFO bi{};
  bi.bmiHeader = {sizeof(BITMAPINFOHEADER), t.w, -t.h, 1, 32, BI_RGB};
  void* bits = nullptr;
  HDC screen = GetDC(nullptr), mem = CreateCompatibleDC(screen);
  HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  HGDIOBJ old = SelectObject(mem, bmp);
  try {
    const auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    winrt::com_ptr<ID2D1DCRenderTarget> rt;
    winrt::check_hresult(t.d2d->CreateDCRenderTarget(&props, rt.put()));
    const RECT r{0, 0, t.w, t.h};
    winrt::check_hresult(rt->BindDC(mem, &r));
    auto format = [&](float size, DWRITE_FONT_WEIGHT weight) {
      winrt::com_ptr<IDWriteTextFormat> f;
      winrt::check_hresult(t.dw->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                                  DWRITE_FONT_STRETCH_NORMAL, Px(size), L"", f.put()));
      f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
      winrt::com_ptr<IDWriteInlineObject> dots;
      t.dw->CreateEllipsisTrimmingSign(f.get(), dots.put());
      DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
      f->SetTrimming(&trim, dots.get());
      return f;
    };
    const auto title_fmt = format(15, DWRITE_FONT_WEIGHT_SEMI_BOLD), detail_fmt = format(12.5f, DWRITE_FONT_WEIGHT_NORMAL);
    winrt::com_ptr<ID2D1SolidColorBrush> card, edge, white, soft, mark;
    rt->CreateSolidColorBrush(D2D1::ColorF(ok ? 0xe83030 : 0x2a2f3a), card.put());
    rt->CreateSolidColorBrush(D2D1::ColorF(0xffffff, 0.16f), edge.put());
    rt->CreateSolidColorBrush(D2D1::ColorF(0xffffff), white.put());
    rt->CreateSolidColorBrush(D2D1::ColorF(0xffffff, 0.85f), soft.put());
    rt->CreateSolidColorBrush(D2D1::ColorF(ok ? 0xe83030 : 0x2a2f3a), mark.put());
    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0, 0));
    const float o = Px(0.5f);
    const auto box = D2D1::RoundedRect(D2D1::RectF(o, o, W - o, H - o), Px(12), Px(12));
    rt->FillRoundedRectangle(box, card.get());
    rt->DrawRoundedRectangle(box, edge.get(), Px(1));
    // a white disc with a check (saved) or a cross (not)
    const D2D1_POINT_2F c{Px(30), H / 2};
    rt->FillEllipse(D2D1::Ellipse(c, Px(12), Px(12)), white.get());
    winrt::com_ptr<ID2D1PathGeometry> path;
    t.d2d->CreatePathGeometry(path.put());
    winrt::com_ptr<ID2D1GeometrySink> sink;
    path->Open(sink.put());
    if (ok) {
      sink->BeginFigure({c.x - Px(5.5f), c.y + Px(0.5f)}, D2D1_FIGURE_BEGIN_HOLLOW);
      sink->AddLine({c.x - Px(1.5f), c.y + Px(4.5f)});
      sink->AddLine({c.x + Px(5.5f), c.y - Px(4)});
      sink->EndFigure(D2D1_FIGURE_END_OPEN);
    } else {
      for (float s : {1.f, -1.f}) {
        sink->BeginFigure({c.x - Px(4.5f), c.y - Px(4.5f) * s}, D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddLine({c.x + Px(4.5f), c.y + Px(4.5f) * s});
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
      }
    }
    sink->Close();
    winrt::com_ptr<ID2D1StrokeStyle> round;
    t.d2d->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                         D2D1_LINE_JOIN_ROUND), nullptr, 0, round.put());
    rt->DrawGeometry(path.get(), mark.get(), Px(2.4f), round.get());
    const float x = Px(54), right = W - Px(14);
    rt->DrawText(title.c_str(), UINT32(title.size()), title_fmt.get(), D2D1::RectF(x, Px(10), right, Px(31)), white.get());
    rt->DrawText(detail.c_str(), UINT32(detail.size()), detail_fmt.get(), D2D1::RectF(x, Px(31), right, Px(50)), soft.get());
    winrt::check_hresult(rt->EndDraw());
    if (t.capture) t.capture->assign(static_cast<uint8_t*>(bits), static_cast<uint8_t*>(bits) + size_t(t.w) * t.h * 4);
    POINT src{0, 0}, at = t.at;
    SIZE size{t.w, t.h};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
    UpdateLayeredWindow(t.wnd, screen, &at, &size, mem, &src, 0, &blend, ULW_ALPHA);
  } catch (const winrt::hresult_error& e) {
    Log("toast: {}", winrt::to_string(e.message()));
  }
  SelectObject(mem, old);
  DeleteObject(bmp);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
}
}  // namespace

void ShowToast(HWND over, const std::wstring& title, const std::wstring& detail, bool ok, bool waiting) {
  if (!Ready()) return;
  const bool showing = IsWindowVisible(t.wnd);
  if (!showing) {  // placed on the game's monitor, sized for its DPI
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(over ? over : GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY), &mi);
    t.dpi = std::max(96.f, float(over ? GetDpiForWindow(over) : GetDpiForSystem()));
    t.at = {mi.rcMonitor.right - LONG(std::ceil(Px(kMargin + kW))), mi.rcMonitor.top + LONG(Px(kMargin))};
  }
  Paint(title, detail, ok, showing ? 255 : 0);
  t.hold = waiting ? kWait : kHold;
  // Already up: new text in place, and the wait starts over. Otherwise slide in.
  t.since = showing ? GetTickCount64() - ULONGLONG(kIn) : GetTickCount64();
  SetWindowPos(t.wnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
  Place(double(GetTickCount64() - t.since));
  SetTimer(t.wnd, kTick, 15, nullptr);
}

bool ToastSnapshot(const std::filesystem::path& bmp, const std::wstring& title, const std::wstring& detail, bool ok,
                   float dpi) {
  if (!Ready()) return false;
  std::vector<uint8_t> px;
  t.dpi = dpi;
  t.capture = &px;
  Paint(title, detail, ok, 0);
  t.capture = nullptr;
  if (px.empty()) return false;
  BITMAPV5HEADER h{};  // 32-bit with alpha, premultiplied as painted
  h.bV5Size = sizeof h;
  h.bV5Width = t.w;
  h.bV5Height = -t.h;
  h.bV5Planes = 1;
  h.bV5BitCount = 32;
  h.bV5Compression = BI_BITFIELDS;
  h.bV5RedMask = 0x00ff0000, h.bV5GreenMask = 0x0000ff00, h.bV5BlueMask = 0x000000ff, h.bV5AlphaMask = 0xff000000;
  h.bV5CSType = LCS_sRGB;
  BITMAPFILEHEADER f{0x4d42, DWORD(sizeof f + sizeof h + px.size()), 0, 0, DWORD(sizeof f + sizeof h)};
  std::ofstream o(bmp, std::ios::binary);
  o.write(reinterpret_cast<const char*>(&f), sizeof f).write(reinterpret_cast<const char*>(&h), sizeof h);
  o.write(reinterpret_cast<const char*>(px.data()), std::streamsize(px.size()));
  return bool(o);
}

void HideToast() {
  if (!t.wnd) return;
  KillTimer(t.wnd, kTick);
  ShowWindow(t.wnd, SW_HIDE);
}
