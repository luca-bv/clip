#include "session.h"

#include <algorithm>

namespace fs = std::filesystem;

PacketRing::~PacketRing() {
  for (auto& t : tracks_) {
    for (auto* p : t->pkts) av_packet_free(&p);
    avcodec_parameters_free(&t->par);
  }
}

int PacketRing::AddTrack(std::string title, const AVCodecContext* enc, bool video) {
  auto t = std::make_unique<Track>(std::move(title), avcodec_parameters_alloc(), enc->time_base, video);
  avcodec_parameters_from_context(t->par, enc);
  std::lock_guard l(m_);
  tracks_.push_back(std::move(t));
  return int(tracks_.size() - 1);
}

void PacketRing::SetKeepSeconds(int s) {
  std::lock_guard l(m_);
  keep_us_ = (s + 2) * int64_t(1000000);  // margin for the save delay and the GOP before the start
}

void PacketRing::Drain(int track, AVCodecContext* enc) {
  for (;;) {
    AVPacket* p = av_packet_alloc();
    if (avcodec_receive_packet(enc, p) < 0) {
      av_packet_free(&p);
      return;
    }
    std::lock_guard l(m_);
    auto& t = *tracks_[track];
    t.pkts.push_back(p);
    if (t.video) Evict(Us(t, p->dts));
  }
}

void PacketRing::Evict(int64_t now_us) {
  const int64_t cutoff = now_us - keep_us_;
  for (auto& t : tracks_) {
    auto& q = t->pkts;
    if (t->video) {
      // Drop whole GOPs while the next GOP still starts before the cutoff, so a keyframe always leads.
      for (;;) {
        size_t k = 1;
        while (k < q.size() && !(q[k]->flags & AV_PKT_FLAG_KEY)) k++;
        if (k >= q.size() || Us(*t, q[k]->pts) > cutoff) break;
        for (; k; k--) {
          av_packet_free(&q.front());
          q.pop_front();
        }
      }
    } else {
      while (!q.empty() && Us(*t, q.front()->pts) < cutoff) {
        av_packet_free(&q.front());
        q.pop_front();
      }
    }
  }
}

std::string PacketRing::Export(int64_t end_us, int seconds, const fs::path& file) {
  struct Out {
    std::string title;
    AVCodecParameters* par;
    AVRational tb;
    std::vector<AVPacket*> pkts;
  };
  std::vector<Out> outs;
  int64_t start_us = 0;
  {
    std::lock_guard l(m_);
    auto v = std::ranges::find_if(tracks_, [](auto& t) { return t->video; });
    if (v == tracks_.end()) return "nothing recorded yet";
    auto& vq = (*v)->pkts;
    // Start at the last keyframe at or before the wanted start, or the oldest one we have.
    const int64_t want = end_us - seconds * int64_t(1000000);
    size_t ks = vq.size();
    for (size_t i = 0; i < vq.size(); i++) {
      if (!(vq[i]->flags & AV_PKT_FLAG_KEY)) continue;
      if (ks != vq.size() && Us(**v, vq[i]->pts) > want) break;
      ks = i;
    }
    if (ks == vq.size()) return "nothing recorded yet";
    start_us = Us(**v, vq[ks]->pts);

    for (auto& t : tracks_) {
      Out o{t->title, nullptr, t->tb, {}};
      for (size_t i = t->video ? ks : 0; i < t->pkts.size(); i++) {
        auto* p = t->pkts[i];
        if (t->video ? Us(*t, p->dts) > end_us : Us(*t, p->pts) > end_us) break;
        if (!t->video && Us(*t, p->pts) < start_us) continue;
        o.pkts.push_back(av_packet_clone(p));
      }
      if (o.pkts.empty()) continue;
      o.par = avcodec_parameters_alloc();
      avcodec_parameters_copy(o.par, t->par);
      outs.push_back(std::move(o));
    }
  }

  AVFormatContext* oc = nullptr;
  auto tmp = file;
  tmp += L".part";
  const std::string tmp8 = Utf8(tmp.wstring());
  std::string err;
  try {
    CheckAv(avformat_alloc_output_context2(&oc, nullptr, "mp4", tmp8.c_str()), "create mp4");
    oc->avoid_negative_ts = AVFMT_AVOID_NEG_TS_MAKE_ZERO;  // one shift for all streams keeps them in sync
    for (auto& o : outs) {
      AVStream* st = avformat_new_stream(oc, nullptr);
      avcodec_parameters_copy(st->codecpar, o.par);
      st->codecpar->codec_tag = 0;
      st->time_base = o.tb;
      av_dict_set(&st->metadata, "title", o.title.c_str(), 0);
      av_dict_set(&st->metadata, "handler_name", o.title.c_str(), 0);
      st->disposition = (o.par->codec_type == AVMEDIA_TYPE_VIDEO || o.title == "Mix") ? AV_DISPOSITION_DEFAULT : 0;
    }
    CheckAv(avio_open(&oc->pb, tmp8.c_str(), AVIO_FLAG_WRITE), "open file");
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "movflags", "+faststart", 0);
    int r = avformat_write_header(oc, &opts);
    av_dict_free(&opts);
    CheckAv(r, "write header");

    struct Item {
      int64_t dts_us;
      int stream;
      AVPacket* p;
    };
    std::vector<Item> items;
    for (int s = 0; s < int(outs.size()); s++)
      for (auto*& p : outs[s].pkts) {
        items.push_back({av_rescale_q(p->dts, outs[s].tb, {1, 1000000}), s, p});
        p = nullptr;  // owned by items now
      }
    std::ranges::stable_sort(items, {}, &Item::dts_us);
    std::vector<int64_t> last_dts(outs.size(), INT64_MIN);
    for (auto& it : items) {
      auto& o = outs[it.stream];
      AVStream* st = oc->streams[it.stream];
      const int64_t off = av_rescale_q(start_us, {1, 1000000}, o.tb);
      it.p->pts -= off;
      it.p->dts -= off;
      it.p->stream_index = it.stream;
      av_packet_rescale_ts(it.p, o.tb, st->time_base);
      // A source toggled off and on restarts its encoder; skip any overlap rather than fail the mux.
      if (it.p->dts > last_dts[it.stream]) {
        last_dts[it.stream] = it.p->dts;
        if (err.empty()) {
          r = av_interleaved_write_frame(oc, it.p);
          if (r < 0) err = "write failed";
        }
      }
      av_packet_free(&it.p);
    }
    if (err.empty()) CheckAv(av_write_trailer(oc), "finish mp4");
  } catch (const std::exception& e) {
    err = e.what();
  }
  for (auto& o : outs) {
    for (auto* p : o.pkts) av_packet_free(&p);
    avcodec_parameters_free(&o.par);
  }
  if (oc) {
    avio_closep(&oc->pb);
    avformat_free_context(oc);
  }

  if (err.empty()) {  // make sure it opens before calling it saved
    AVFormatContext* ic = nullptr;
    if (avformat_open_input(&ic, tmp8.c_str(), nullptr, nullptr) < 0 || avformat_find_stream_info(ic, nullptr) < 0 ||
        ic->nb_streams != outs.size())
      err = "saved file doesn't open";
    avformat_close_input(&ic);
  }
  if (err.empty() && !MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING)) err = "rename failed";
  if (!err.empty()) DeleteFileW(tmp.c_str());
  Log("save {} ({} tracks, from {:.1f}s): {}", Utf8(file.filename().wstring()), outs.size(), start_us / 1e6,
      err.empty() ? "ok" : err);
  return err;
}

Session::Session(HWND hwnd_, DWORD pid_, std::string exe_, const Config& cfg, std::function<void()> on_window_closed)
    : pid(pid_), exe(std::move(exe_)), hwnd(hwnd_), t0_(QpcNow()), ring_(std::make_shared<PacketRing>()) {
  ring_->SetKeepSeconds(cfg.clip_seconds);
  video_ = std::make_unique<VideoCapture>(hwnd, cfg, *ring_, t0_, std::move(on_window_closed));  // track 0
  audio_ = std::make_unique<AudioMixer>(*ring_, t0_);                                           // track 1 = mix
  SyncAudio(cfg);
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

void Session::Save(int seconds, fs::path file, std::function<void(std::string)> done) {
  const int64_t end_us = QpcTo(QpcNow() - t0_, 1000000);
  std::thread([ring = ring_, end_us, seconds, file = std::move(file), done = std::move(done)] {
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);  // low CPU and I/O priority
    Sleep(500);  // let the encoders catch up to the hotkey moment
    done(ring->Export(end_us, seconds, file));
  }).detach();
}
