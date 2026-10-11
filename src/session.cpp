#include "session.h"

#include <algorithm>

namespace fs = std::filesystem;

Session::Session(HWND hwnd_, DWORD pid_, std::string exe_, const Config& cfg, std::function<void()> on_window_closed)
    : pid(pid_), exe(std::move(exe_)), hwnd(hwnd_), t0_(QpcNow()), ring_(std::make_shared<PacketRing>()) {
  ring_->SetKeepSeconds(cfg.clip_seconds);
  video_ = std::make_unique<VideoCapture>(hwnd, cfg, *ring_, t0_, std::move(on_window_closed));  // track 0
  audio_ = std::make_unique<AudioMixer>(*ring_, t0_);                                           // track 1 = mix
  SyncAudio(cfg);
  SetOverlay(cfg.overlay);
  Log("recording {}", exe);
}

Session::~Session() { Log("stopped recording {}", exe); }

void Session::SetWindow(HWND w) {
  hwnd = w;
  video_->SetWindow(w);
}

void Session::SyncAudio(const Config& cfg) {
  std::vector<std::pair<std::string, DWORD>> want;
  if (cfg.record_game_audio) want.emplace_back("Game", pid);
  if (cfg.record_mic) want.emplace_back("Microphone", 0);
  if (!cfg.apps.empty()) {
    auto roots = ProcessRoots();
    for (auto& a : cfg.apps)
      if (auto it = roots.find(a); a != exe && it != roots.end()) want.emplace_back(Stem(a), it->second);
  }
  audio_->Sync(want);
}

void Session::SetOverlay(const OverlayConfig& cfg) {
  if (!cfg.enabled) {
    if (overlay_) video_->SetOverlay(nullptr);
    overlay_ = nullptr;
  } else if (!overlay_ || overlay_->config() != cfg) {
    overlay_ = std::make_shared<InputOverlay>(cfg, pid);
    video_->SetOverlay(overlay_);
  }
}

void Session::Save(int seconds, fs::path file, std::function<void(std::string)> done) {
  const int64_t end_us = QpcTo(QpcNow() - t0_, 1000000);
  std::thread([ring = ring_, end_us, seconds, file = std::move(file), done = std::move(done)] {
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);  // low CPU and I/O priority
    Sleep(500);  // let the encoders catch up to the hotkey moment
    done(ring->Export(end_us, seconds, file));
  }).detach();
}
