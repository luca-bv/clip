#include "fifo.h"
#include "session.h"

#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <tlhelp32.h>

#include <algorithm>

namespace {
constexpr int64_t kLatency = kRate * 3 / 20;  // mix 150 ms behind real time so every source has delivered
const WAVEFORMATEX kFormat{WAVE_FORMAT_IEEE_FLOAT, 2, kRate, kRate * 8, 8, 32, 0};
const WAVEFORMATEX kPcm16{WAVE_FORMAT_PCM, 2, kRate, kRate * 4, 4, 16, 0};

struct ActivateHandler : winrt::implements<ActivateHandler, IActivateAudioInterfaceCompletionHandler> {
  winrt::handle done{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
  HRESULT hr = E_FAIL;
  com_ptr<IAudioClient> client;
  STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* op) override {
    com_ptr<IUnknown> unk;
    HRESULT ahr = E_FAIL;
    hr = op->GetActivateResult(&ahr, unk.put());
    if (SUCCEEDED(hr)) hr = ahr;
    if (SUCCEEDED(hr)) client = unk.try_as<IAudioClient>();
    SetEvent(done.get());
    return S_OK;
  }
};

com_ptr<IAudioClient> ActivateLoopback(DWORD pid) {
  AUDIOCLIENT_ACTIVATION_PARAMS params{};
  params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
  params.ProcessLoopbackParams.TargetProcessId = pid;
  params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
  PROPVARIANT pv{};
  pv.vt = VT_BLOB;
  pv.blob.cbSize = sizeof params;
  pv.blob.pBlobData = reinterpret_cast<BYTE*>(&params);
  auto handler = winrt::make_self<ActivateHandler>();
  com_ptr<IActivateAudioInterfaceAsyncOperation> op;
  winrt::check_hresult(ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &pv,
                                                   handler.get(), op.put()));
  if (WaitForSingleObject(handler->done.get(), 5000) != WAIT_OBJECT_0) throw std::runtime_error("activation timed out");
  winrt::check_hresult(handler->hr);
  return handler->client;
}

// Audio of one process tree, through the audio engine's per-process loopback. Float if it takes it,
// else 16-bit PCM (the format Microsoft's sample uses), converted in the capture loop.
com_ptr<IAudioClient> ProcessLoopback(DWORD pid, bool& pcm16) {
  constexpr DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
  auto client = ActivateLoopback(pid);
  pcm16 = FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 2'000'000, 0, &kFormat, nullptr));
  if (pcm16) {
    client = ActivateLoopback(pid);
    winrt::check_hresult(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 2'000'000, 0, &kPcm16, nullptr));
  }
  return client;
}

std::wstring DefaultMicId() {  // "" if there's none; never throws, Sync runs from a window message
  wchar_t* id = nullptr;
  std::wstring r;
  try {
    auto devices = winrt::create_instance<IMMDeviceEnumerator>(__uuidof(MMDeviceEnumerator));
    com_ptr<IMMDevice> dev;
    if (SUCCEEDED(devices->GetDefaultAudioEndpoint(eCapture, eConsole, dev.put())) && SUCCEEDED(dev->GetId(&id))) r = id;
  } catch (const winrt::hresult_error&) {
  }
  CoTaskMemFree(id);
  return r;
}

com_ptr<IAudioClient> OpenMicrophone(std::wstring& id) {
  auto devices = winrt::create_instance<IMMDeviceEnumerator>(__uuidof(MMDeviceEnumerator));
  com_ptr<IMMDevice> dev;
  winrt::check_hresult(devices->GetDefaultAudioEndpoint(eCapture, eConsole, dev.put()));
  wchar_t* s = nullptr;
  if (SUCCEEDED(dev->GetId(&s))) id = s;
  CoTaskMemFree(s);
  com_ptr<IAudioClient> client;
  winrt::check_hresult(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.put_void()));
  winrt::check_hresult(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                          AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                              AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                          2'000'000, 0, &kFormat, nullptr));
  return client;
}

AVCodecContext* OpenAac() {
  const AVCodec* c = avcodec_find_encoder(AV_CODEC_ID_AAC);
  AVCodecContext* e = avcodec_alloc_context3(c);
  e->sample_fmt = AV_SAMPLE_FMT_FLTP;
  e->sample_rate = kRate;
  av_channel_layout_default(&e->ch_layout, 2);
  e->bit_rate = 160000;
  e->time_base = {1, kRate};
  e->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  int r = avcodec_open2(e, c, nullptr);
  if (r < 0) avcodec_free_context(&e);
  CheckAv(r, "open AAC encoder");
  return e;
}

void EncodeFrame(PacketRing& ring, int track, AVCodecContext* e, const float* stereo, int64_t pts) {
  AVFrame* f = av_frame_alloc();
  f->nb_samples = kFrame;
  f->format = AV_SAMPLE_FMT_FLTP;
  f->sample_rate = kRate;
  av_channel_layout_copy(&f->ch_layout, &e->ch_layout);
  if (av_frame_get_buffer(f, 0) >= 0) {
    auto *l = reinterpret_cast<float*>(f->data[0]), *r = reinterpret_cast<float*>(f->data[1]);
    for (int i = 0; i < kFrame; i++) l[i] = stereo[2 * i], r[i] = stereo[2 * i + 1];
    f->pts = pts;
    if (avcodec_send_frame(e, f) >= 0) ring.Drain(track, e);
  }
  av_frame_free(&f);
}
}  // namespace

struct AudioMixer::Source {
  Source(std::string name_, DWORD pid_, int64_t t0)
      : name(std::move(name_)), pid(pid_), t0_hns(QpcTo(t0, 10'000'000)) {
    client = pid ? ProcessLoopback(pid, pcm16) : OpenMicrophone(device);
    winrt::check_hresult(client->GetService(__uuidof(IAudioCaptureClient), capture.put_void()));
    winrt::check_hresult(client->SetEventHandle(event.get()));
    enc = OpenAac();
    try {
      winrt::check_hresult(client->Start());
      thread = std::thread(&Source::Loop, this);
    } catch (...) {  // no destructor for a half-built source
      avcodec_free_context(&enc);
      throw;
    }
  }
  ~Source() {
    stop = true;
    SetEvent(event.get());
    thread.join();
    client->Stop();
    avcodec_free_context(&enc);
  }

  void Loop() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    while (!stop) {
      WaitForSingleObject(event.get(), 100);
      UINT32 n = 0;
      HRESULT hr;
      while (SUCCEEDED(hr = capture->GetNextPacketSize(&n)) && n) {
        BYTE* data;
        UINT32 frames;
        DWORD flags;
        UINT64 qpc = 0;  // 100 ns units
        if (FAILED(hr = capture->GetBuffer(&data, &frames, &flags, nullptr, &qpc))) break;
        if (!qpc) qpc = QpcTo(QpcNow(), 10'000'000) - frames * 10'000'000ull / kRate;
        const int64_t at = (int64_t(qpc) - t0_hns) * kRate / 10'000'000;
        const float* samples = reinterpret_cast<const float*>(data);
        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
          samples = nullptr;
        } else if (pcm16) {
          convert.resize(frames * 2);
          auto* in = reinterpret_cast<const int16_t*>(data);
          for (size_t i = 0; i < convert.size(); i++) convert[i] = in[i] / 32768.f;
          samples = convert.data();
        }
        fifo.Write(at, samples, frames);
        capture->ReleaseBuffer(frames);
      }
      if (FAILED(hr)) {  // device unplugged or process gone; Sync replaces dead sources
        Log("audio source {} stopped (0x{:08x})", name, uint32_t(hr));
        dead = true;
        return;
      }
    }
  }

  const std::string name;
  const DWORD pid;
  const int64_t t0_hns;
  int track = -1;
  AVCodecContext* enc = nullptr;
  std::atomic<bool> stop{false}, dead{false};
  SampleFifo fifo;
  std::wstring device;  // microphone endpoint, to notice when the default changes

 private:
  com_ptr<IAudioClient> client;
  com_ptr<IAudioCaptureClient> capture;
  winrt::handle event{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
  bool pcm16 = false;
  std::vector<float> convert;
  std::thread thread;
};

AudioMixer::AudioMixer(PacketRing& ring, int64_t t0) : ring_(ring), t0_(t0) {
  mix_enc_ = OpenAac();
  mix_track_ = ring_.AddTrack("Mix", mix_enc_, false);
  thread_ = std::thread(&AudioMixer::Loop, this);
}

AudioMixer::~AudioMixer() {
  stop_ = true;
  thread_.join();
  sources_.clear();
  avcodec_free_context(&mix_enc_);
}

void AudioMixer::Sync(const std::vector<std::pair<std::string, DWORD>>& want) {
  const bool mic = std::ranges::any_of(want, [](auto& w) { return w.second == 0; });
  const std::wstring mic_id = mic ? DefaultMicId() : L"";
  auto wanted = [&](const Source& s) {
    return !s.dead && (s.pid || s.device == mic_id) && std::ranges::find(want, std::pair{s.name, s.pid}) != want.end();
  };
  std::vector<std::unique_ptr<Source>> drop;
  {
    std::lock_guard l(m_);
    for (auto& s : sources_)
      if (!wanted(*s)) drop.push_back(std::move(s));
    std::erase(sources_, nullptr);
  }
  for (auto& s : drop) Log("stopped audio: {}", s->name);
  drop.clear();  // joins capture threads outside the lock

  for (auto& [name, pid] : want) {
    {
      std::lock_guard l(m_);
      if (std::ranges::any_of(sources_, [&](auto& s) { return s->name == name; })) continue;
    }
    auto fail = [&](const std::string& why) {  // retried every sync, logged once
      if (std::ranges::find(failed_, name) != failed_.end()) return;
      Log("can't capture audio from {}: {}", name, why);
      failed_.push_back(name);
    };
    try {
      auto s = std::make_unique<Source>(name, pid, t0_);
      auto [it, added] = tracks_.try_emplace(name, -1);
      if (added) it->second = ring_.AddTrack(name, s->enc, false);
      s->track = it->second;
      std::lock_guard l(m_);
      sources_.push_back(std::move(s));
      Log("recording audio: {}", name);
      std::erase(failed_, name);
    } catch (const winrt::hresult_error& e) {
      fail(winrt::to_string(e.message()));
    } catch (const std::exception& e) {
      fail(e.what());
    }
  }
}

void AudioMixer::Loop() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
  std::vector<float> mix(kFrame * 2), tmp(kFrame * 2);
  for (int64_t k = 0; !stop_;) {  // k = next AAC frame on the session timeline
    Sleep(10);
    const int64_t ready = QpcTo(QpcNow() - t0_, kRate) - kLatency;
    if (ready - k * kFrame > 2 * kRate) k = ready / kFrame - 1;  // the PC slept: skip the gap, don't encode it
    while ((k + 1) * kFrame <= ready && !stop_) {
      std::ranges::fill(mix, 0.f);
      {
        std::lock_guard l(m_);
        for (auto& s : sources_) {
          s->fifo.Read(k * kFrame, tmp.data());
          for (size_t i = 0; i < mix.size(); i++) mix[i] += tmp[i];
          EncodeFrame(ring_, s->track, s->enc, tmp.data(), k * kFrame);
        }
      }
      for (auto& x : mix) x = std::clamp(x, -1.f, 1.f);
      EncodeFrame(ring_, mix_track_, mix_enc_, mix.data(), k * kFrame);
      k++;
    }
  }
}

std::map<std::string, DWORD> ProcessRoots() {
  struct Proc {
    std::string exe;
    DWORD parent;
  };
  std::map<DWORD, Proc> procs;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return {};
  PROCESSENTRY32W pe{sizeof pe};
  for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
    procs[pe.th32ProcessID] = {Lower(Utf8(pe.szExeFile)), pe.th32ParentProcessID};
  CloseHandle(snap);
  std::map<std::string, DWORD> roots;
  for (auto& [pid, p] : procs) {
    auto parent = procs.find(p.parent);
    // ponytail: first root wins if an app runs several separate trees.
    if (parent == procs.end() || parent->second.exe != p.exe) roots.try_emplace(p.exe, pid);
  }
  return roots;
}
