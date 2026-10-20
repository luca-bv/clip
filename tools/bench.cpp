// How much recording costs a game. A synthetic game renders flat out on the high-performance GPU
// while VideoCapture records its window, and the game's own frame times are compared per phase.
// Usage: clip_bench [seconds per phase] [clip.mp4]. The window sits off-screen, so nothing shows.
#include <d3dcompiler.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <cstdio>
#include <numeric>

#include "../src/export.h"
#include "../src/preview.h"
#include "../src/session.h"
#include "../src/voice.h"

void LogLine(const std::string& s) {
  std::printf("    clip: %s\n", s.c_str());
  std::fflush(stdout);
}
std::string Utf8(std::wstring_view w) {
  std::string s(WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr), 0);
  WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), int(s.size()), nullptr, nullptr);
  return s;
}
std::wstring Wide(std::string_view s) {
  std::wstring w(MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0), 0);
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), int(w.size()));
  return w;
}

namespace {
constexpr int kW = 1920, kH = 1080;

const char kShader[] = R"(
cbuffer C : register(b0) { float t; uint iters; float2 pad; };
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 p = float2((id << 1) & 2, id & 2);
  return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps(float4 pos : SV_Position) : SV_Target {
  float2 uv = pos.xy / float2(1920, 1080);
  float a = uv.x * 3.1 + uv.y * 1.7 + t;
  for (uint i = 0; i < iters; i++) a = sin(a) * 1.0001 + cos(a * 0.999);
  float bar = abs(uv.x - frac(t * 0.25)) < 0.02 ? 1 : 0;
  return float4(0.25 + 0.1 * sin(a), uv.y * 0.5, bar, 1);
})";

com_ptr<IDXGIAdapter1> FastAdapter() {
  com_ptr<IDXGIFactory6> f;
  winrt::check_hresult(CreateDXGIFactory2(0, IID_PPV_ARGS(f.put())));
  com_ptr<IDXGIAdapter1> a;
  winrt::check_hresult(f->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(a.put())));
  return a;
}

com_ptr<ID3DBlob> Compile(const char* entry, const char* target) {
  com_ptr<ID3DBlob> code, err;
  if (FAILED(D3DCompile(kShader, sizeof kShader - 1, nullptr, nullptr, nullptr, entry, target, 0, 0, code.put(),
                        err.put())))
    throw std::runtime_error(static_cast<const char*>(err->GetBufferPointer()));
  return code;
}

// Renders on its own thread as fast as the GPU allows and records each frame's time.
class Game {
 public:
  explicit Game(HWND wnd) {
    winrt::check_hresult(D3D11CreateDevice(FastAdapter().get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                           D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                                           dev_.put(), nullptr, ctx_.put()));
    com_ptr<IDXGIFactory2> f;
    winrt::check_hresult(CreateDXGIFactory2(0, IID_PPV_ARGS(f.put())));
    DXGI_SWAP_CHAIN_DESC1 d{};
    d.Width = kW;
    d.Height = kH;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    winrt::check_hresult(f->CreateSwapChainForHwnd(dev_.get(), wnd, &d, nullptr, nullptr, sc_.put()));
    com_ptr<ID3D11Texture2D> back;
    winrt::check_hresult(sc_->GetBuffer(0, IID_PPV_ARGS(back.put())));
    winrt::check_hresult(dev_->CreateRenderTargetView(back.get(), nullptr, rtv_.put()));
    auto vs = Compile("vs", "vs_5_0"), ps = Compile("ps", "ps_5_0");
    winrt::check_hresult(dev_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, vs_.put()));
    winrt::check_hresult(dev_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, ps_.put()));
    D3D11_BUFFER_DESC bd{16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE};
    winrt::check_hresult(dev_->CreateBuffer(&bd, nullptr, cb_.put()));
    thread_ = std::thread(&Game::Loop, this);
  }
  ~Game() {
    stop_ = true;
    thread_.join();
  }
  std::atomic<uint32_t> iters{64};

  std::vector<double> Take() {  // frame times in ms since the last call
    std::lock_guard l(m_);
    return std::exchange(times_, {});
  }

 private:
  void Loop() {
    const int64_t f = QpcFreq(), start = QpcNow();
    int64_t last = start;
    while (!stop_) {
      D3D11_MAPPED_SUBRESOURCE m;
      ctx_->Map(cb_.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
      struct {
        float t;
        uint32_t iters, pad[2];
      } c{float(double(QpcNow() - start) / double(f)), iters.load()};
      memcpy(m.pData, &c, sizeof c);
      ctx_->Unmap(cb_.get(), 0);
      auto* rtv = rtv_.get();
      ctx_->OMSetRenderTargets(1, &rtv, nullptr);
      D3D11_VIEWPORT vp{0, 0, kW, kH, 0, 1};
      ctx_->RSSetViewports(1, &vp);
      ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      ctx_->VSSetShader(vs_.get(), nullptr, 0);
      ctx_->PSSetShader(ps_.get(), nullptr, 0);
      auto* cb = cb_.get();
      ctx_->PSSetConstantBuffers(0, 1, &cb);
      ctx_->Draw(3, 0);
      sc_->Present(0, 0);
      const int64_t now = QpcNow();
      std::lock_guard l(m_);
      times_.push_back(double(now - last) * 1000.0 / double(f));
      last = now;
    }
  }

  com_ptr<ID3D11Device> dev_;
  com_ptr<ID3D11DeviceContext> ctx_;
  com_ptr<IDXGISwapChain1> sc_;
  com_ptr<ID3D11RenderTargetView> rtv_;
  com_ptr<ID3D11VertexShader> vs_;
  com_ptr<ID3D11PixelShader> ps_;
  com_ptr<ID3D11Buffer> cb_;
  std::mutex m_;
  std::vector<double> times_;
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

void Pump(double seconds) {
  const int64_t end = QpcNow() + int64_t(seconds * double(QpcFreq()));
  while (QpcNow() < end) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
    Sleep(5);
  }
}

struct Stats {
  double fps, low1, worst;
};
Stats Measure(Game& game, double seconds) {
  game.Take();
  Pump(seconds);
  auto t = game.Take();
  if (t.empty()) return {};
  std::ranges::sort(t, std::greater<>());
  const size_t n1 = std::max<size_t>(1, t.size() / 100);
  const double total = std::accumulate(t.begin(), t.end(), 0.0);
  return {1000.0 * double(t.size()) / total, 1000.0 * double(n1) / std::accumulate(t.begin(), t.begin() + n1, 0.0),
          t.front()};
}

// Scales the shader's work until the game runs near `fps` with nothing else on the GPU.
void Calibrate(Game& game, double fps) {
  for (int i = 0; i < 6; i++) {
    const double got = Measure(game, 1.0).fps;
    const double next = double(game.iters) * got / fps;
    game.iters = uint32_t(std::clamp(next, 1.0, 1e6));
  }
}
}  // namespace

int main(int argc, char** argv) {
  winrt::init_apartment();
  const double phase = argc > 1 ? atof(argv[1]) : 32;
  const std::filesystem::path out = argc > 2 ? argv[2] : "bench.mp4";  // smoke mode too: clip_bench smoke out.mp4

  WNDCLASSW wc{};
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"ClipBenchGame";
  RegisterClassW(&wc);
  const int x = GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN) + 200;
  HWND wnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, wc.lpszClassName, L"Clip bench game", WS_POPUP, x,
                             0, kW, kH, nullptr, nullptr, wc.hInstance, nullptr);
  ShowWindow(wnd, SW_SHOWNOACTIVATE);

  if (argc > 3 && std::string_view(argv[1]) == "preview") {  // clip_bench preview in.mp4 outdir
    const int64_t t = QpcNow();
    const auto err = MakePreview(argv[2], argv[3]);
    std::printf("preview: %s in %.2f s\n", err.empty() ? "ok" : err.c_str(), double(QpcNow() - t) / double(QpcFreq()));
    return err.empty() ? 0 : 1;
  }
  if (argc > 1 && std::string_view(argv[1]) == "voice") {  // listen for the phrase for a few seconds
    {
      VoiceSave v([](double ago) { std::printf("heard it, began %.1f s ago\n", ago); },
                  [](std::string why) { std::printf("failed: %s\n", why.c_str()); });
      Sleep(argc > 2 ? atoi(argv[2]) * 1000 : 4000);
    }
    std::printf("voice: done\n");
    return 0;
  }
  // clip_bench export in.mp4 out.mp4 <all|a-b,c-d> [MB] [audio track, -1 = none] [vertical crop 0..1]
  if (argc > 4 && std::string_view(argv[1]) == "export") {
    ExportOptions o;
    o.in = argv[2];
    o.out = argv[3];
    for (std::string_view parts = argv[4]; parts != "all" && !parts.empty();) {
      const auto part = parts.substr(0, parts.find(','));
      const auto dash = part.find('-');
      o.keep.emplace_back(atof(std::string(part.substr(0, dash)).c_str()), atof(std::string(part.substr(dash + 1)).c_str()));
      parts.remove_prefix(std::min(parts.size(), part.size() + 1));
    }
    o.target_bytes = argc > 5 ? int64_t(atof(argv[5]) * 1e6) : 0;
    o.audio = argc > 6 ? atoi(argv[6]) : 0;
    if (argc > 6 && argv[6][0] == 'g')  // g1:0.5 = game as recorded, mic at half
      for (std::string_view g = argv[6] + 1; !g.empty();) {
        const auto one = g.substr(0, g.find(':'));
        o.gains.push_back(float(atof(std::string(one).c_str())));
        g.remove_prefix(std::min(g.size(), one.size() + 1));
      }
    o.vertical = argc > 7;
    o.crop_x = argc > 7 ? atof(argv[7]) : 0.5;
    if (argc > 8) {  // also print the clip info the editor gets
      const ClipInfo info = ReadClipInfo(o.in);
      std::printf("info: %.2f s %dx%d, %zu tracks, %zu peaks, max %.2f %s\n", info.duration, info.width, info.height,
                  info.tracks.size(), info.peaks.size(), *std::ranges::max_element(info.peaks), info.error.c_str());
    }
    std::atomic<bool> cancel{false};
    const int64_t t = QpcNow();
    const auto err = ExportClip(o, [](double) {}, cancel);
    std::printf("export: %s in %.2f s\n", err.empty() ? "ok" : err.c_str(), double(QpcNow() - t) / double(QpcFreq()));
    return err.empty() ? 0 : 1;
  }
  try {
    Game game(wnd);
    Pump(2);
    if (argc > 1 && std::string_view(argv[1]) == "smoke") {  // light load: does recording work at all here?
      game.iters = 8;
      PacketRing ring;
      ring.SetKeepSeconds(10);
      const int64_t t0 = QpcNow();
      Config cfg;
      VideoCapture video(wnd, cfg, ring, t0, [] {});
      Pump(4);
      const auto err = ring.Export(QpcTo(QpcNow() - t0, 1000000), 3, out);
      std::printf("smoke: %s\n", err.empty() ? out.string().c_str() : err.c_str());
      DestroyWindow(wnd);
      return err.empty() ? 0 : 1;
    }
    std::printf("%-34s %8s %10s %10s\n", "phase", "avg fps", "1% low", "worst ms");
    auto row = [](const std::string& name, Stats s) {
      std::printf("%-34s %8.1f %10.1f %10.2f\n", name.c_str(), s.fps, s.low1, s.worst);
      std::fflush(stdout);
    };
    for (double target : {144.0, 45.0}) {
      Calibrate(game, target);
      const std::string load = std::format("{:.0f} fps game", target);
      row(load + ", not recording", Measure(game, phase));
      for (int prio : {0, -7}) {
        Config cfg;
        cfg.gpu_priority = prio;
        PacketRing ring;
        ring.SetKeepSeconds(int(phase) + 5);
        const int64_t t0 = QpcNow();
        VideoCapture video(wnd, cfg, ring, t0, [] {});
        Pump(1);
        row(std::format("{}, recording, GPU priority {}", load, prio), Measure(game, phase));
        if (target == 45.0 && prio == -7) {
          Pump(0.5);
          const auto err = ring.Export(QpcTo(QpcNow() - t0, 1000000), 10, out);
          std::printf("clip export: %s\n", err.empty() ? out.string().c_str() : err.c_str());
        }
      }
      row(load + ", not recording again", Measure(game, phase));
    }
  } catch (const std::exception& e) {
    std::printf("failed: %s\n", e.what());
    return 1;
  } catch (const winrt::hresult_error& e) {
    std::printf("failed: %s\n", winrt::to_string(e.message()).c_str());
    return 1;
  }
  DestroyWindow(wnd);
  return 0;
}
