#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <vector>

constexpr int kRate = 48000;
constexpr int kFrame = 1024;             // AAC frame
constexpr int64_t kJitter = kRate / 50;  // timestamps within 20 ms of the expected position count as contiguous

// Stereo float samples placed on the session timeline (sample index at kRate). A capture thread
// writes packets with timestamps; the mixer reads fixed frames, getting silence where nothing arrived.
class SampleFifo {
 public:
  // Place `frames` samples (nullptr = silence) at timeline position `at`, padding gaps and dropping overlaps.
  void Write(int64_t at, const float* s, uint32_t frames) {
    std::lock_guard l(m_);
    if (start_ < 0) start_ = at;
    const int64_t end = start_ + int64_t(buf_.size() / 2);
    if (std::abs(at - end) <= kJitter) at = end;
    if (at > end) buf_.resize(buf_.size() + size_t(at - end) * 2, 0.f);
    const int64_t skip = std::max<int64_t>(end - at, 0);
    if (skip >= frames) return;
    if (s) buf_.insert(buf_.end(), s + skip * 2, s + frames * 2);
    else buf_.resize(buf_.size() + size_t(frames - skip) * 2, 0.f);
    if (buf_.size() > kRate * 4) {  // nobody is reading; keep 2 s
      const size_t drop = buf_.size() - kRate * 4;
      buf_.erase(buf_.begin(), buf_.begin() + drop);
      start_ += int64_t(drop / 2);
    }
  }

  // Copy timeline [at, at + kFrame) into `out` (kFrame * 2 floats), silence where nothing was captured.
  void Read(int64_t at, float* out) {
    std::fill_n(out, kFrame * 2, 0.f);
    std::lock_guard l(m_);
    if (start_ < 0) return;
    if (at > start_) {  // discard what's older than the frame
      const size_t drop = std::min(size_t(at - start_) * 2, buf_.size());
      buf_.erase(buf_.begin(), buf_.begin() + drop);
      start_ = buf_.empty() ? at : start_ + int64_t(drop / 2);
    }
    const int64_t off = start_ - at;
    if (off >= kFrame) return;
    const size_t take = std::min(size_t(kFrame - off) * 2, buf_.size());
    std::copy_n(buf_.begin(), take, out + off * 2);
    buf_.erase(buf_.begin(), buf_.begin() + take);
    start_ += int64_t(take / 2);
    if (buf_.empty()) start_ = std::max(start_, at + kFrame);
  }

 private:
  std::mutex m_;
  std::vector<float> buf_;  // interleaved stereo
  int64_t start_ = -1;      // timeline position of buf_[0]
};
