#pragma once
#include "common.h"

#include <unknwn.h>  // before C++/WinRT, so it can implement classic COM interfaces

#include <audioclient.h>
#include <d3d11_4.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <atomic>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "gpu.h"
#include "overlay.h"
#include "ring.h"


using winrt::com_ptr;

// WGC capture of one window -> NV12 on the GPU -> hardware H.264 at a steady frame rate.
class VideoCapture {
 public:
  VideoCapture(HWND hwnd, const Config& cfg, PacketRing& ring, int64_t t0, std::function<void()> on_closed);
  ~VideoCapture();
  void SetWindow(HWND hwnd);  // the game recreated its window
  void SetOverlay(std::shared_ptr<InputOverlay> overlay);
  bool failed() const { return failed_; }

 private:
  void CreateDevice(int gpu_priority);
  void OpenEncoder(const Config& cfg);
  void StartCapture(winrt::Windows::Graphics::Capture::GraphicsCaptureItem item);
  void StopCapture();
  void Loop();
  bool Encode(int64_t pts);
  void CreateProcessor(UINT in_w, UINT in_h);
  ID3D11VideoProcessorInputView* InputView(ID3D11Texture2D* tex);
  void CopyFrame(ID3D11Texture2D* tex);
  void DrawOverlay(InputOverlay& overlay);
  void EnsureD2D();
  void Free();

  PacketRing& ring_;
  int track_ = -1;
  int64_t t0_;
  int fps_;
  int w_ = 0, h_ = 0;
  std::function<void()> on_closed_;

  com_ptr<ID3D11Device> dev_;
  com_ptr<ID3D11DeviceContext> ctx_;
  com_ptr<ID3D11VideoDevice> vdev_;
  com_ptr<ID3D11VideoContext1> vctx_;
  com_ptr<ID3D11VideoProcessorEnumerator> venum_;
  com_ptr<ID3D11VideoProcessor> vp_;
  UINT vp_w_ = 0, vp_h_ = 0;
  std::vector<std::pair<com_ptr<ID3D11Texture2D>, com_ptr<ID3D11VideoProcessorInputView>>> in_views_;
  OutputViews out_views_;
  com_ptr<ID3D11Texture2D> copy_;  // frame copy to draw the overlay on, or if the processor can't read WGC textures
  bool use_copy_ = false;
  com_ptr<ID2D1DeviceContext> d2d_;
  com_ptr<ID2D1Bitmap1> d2d_target_;  // copy_ as a Direct2D target


  winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice rt_dev_{nullptr};
  winrt::Windows::Graphics::Capture::GraphicsCaptureItem item_{nullptr};
  winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool pool_{nullptr};
  winrt::Windows::Graphics::Capture::GraphicsCaptureSession session_{nullptr};
  winrt::event_token closed_token_{};

  std::mutex frame_m_;
  winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame latest_{nullptr};
  std::shared_ptr<InputOverlay> overlay_;
  winrt::Windows::Graphics::SizeInt32 pool_size_{};

  AVBufferRef* hw_dev_ = nullptr;
  AVBufferRef* hw_frames_ = nullptr;
  AVCodecContext* enc_ = nullptr;
  AVFrame* last_ = nullptr;  // last converted surface, re-sent when the game shows the same frame
  bool reuse_ = false;

  int64_t last_frame_time_ = -1, repeats_ = 0;  // video thread only
  std::atomic<bool> stop_{false}, failed_{false};
  std::thread thread_;
};

// Captures each audio source, mixes them, and encodes one AAC track per source plus a mix track.
class AudioMixer {
 public:
  AudioMixer(PacketRing& ring, int64_t t0);
  ~AudioMixer();
  // Make the captured set match `want`: {track name, process id (0 = microphone)}.
  void Sync(const std::vector<std::pair<std::string, DWORD>>& want);

  struct Source;

 private:
  void Loop();

  PacketRing& ring_;
  int64_t t0_;
  AVCodecContext* mix_enc_ = nullptr;
  int mix_track_ = -1;
  std::map<std::string, int> tracks_;  // name -> ring track, reused when a source is toggled back on
  std::vector<std::string> failed_;     // sources we already logged a failure for

  std::mutex m_;
  std::vector<std::unique_ptr<Source>> sources_;
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

// Lowercase exe name -> pid of its top-most process (the one whose parent is a different exe).
std::map<std::string, DWORD> ProcessRoots();

// Everything recording one game.
class Session {
 public:
  Session(HWND hwnd, DWORD pid, std::string exe, const Config& cfg, std::function<void()> on_window_closed);
  ~Session();
  void SetWindow(HWND w);
  void SetClipSeconds(int s) { ring_->SetKeepSeconds(s); }
  void SyncAudio(const Config& cfg);
  void SetOverlay(const OverlayConfig& cfg);
  bool failed() const { return video_->failed(); }
  // Saves `seconds` ending `end_ago` seconds back to `file` in the background; `done` gets "" or an error.
  void Save(int seconds, std::filesystem::path file, std::function<void(std::string)> done, double end_ago = 0);

  const DWORD pid;
  const std::string exe;
  HWND hwnd;

 private:
  int64_t t0_;
  std::shared_ptr<PacketRing> ring_;  // shared with saves still running after the session ends
  std::unique_ptr<VideoCapture> video_;
  std::unique_ptr<AudioMixer> audio_;
  std::shared_ptr<InputOverlay> overlay_;
};
