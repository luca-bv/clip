#pragma once
#include <cmath>
#include <cstdint>
#include <deque>

// Path of the mouse in an unbounded virtual plane (key widths), trimmed by age and by length.
// The overlay maps it into the pad, either wrapping at the edges or panning around the newest point.
class MouseTrail {
 public:
  struct Pt {
    float x, y;
    int64_t t;  // any monotonic clock
    bool m1;    // left button held
  };

  // Points closer than `merge` in time update the newest one, so fast mice don't flood the trail.
  void Move(float dx, float dy, int64_t t, bool m1, int64_t merge) {
    x_ += dx;
    y_ += dy;
    if (!pts_.empty() && t - pts_.back().t < merge) {
      pts_.back() = {x_, y_, pts_.back().t, pts_.back().m1 || m1};
    } else {
      pts_.push_back({x_, y_, t, m1});
      if (pts_.size() > 4000) pts_.pop_front();
    }
  }

  // Drops points older than `max_age` and anything further than `max_len` along the path from the newest.
  void Trim(int64_t now, int64_t max_age, float max_len) {
    while (!pts_.empty() && now - pts_.front().t > max_age) pts_.pop_front();
    float len = 0;
    for (size_t i = pts_.size(); i-- > 1;) {
      len += std::hypot(pts_[i].x - pts_[i - 1].x, pts_[i].y - pts_[i - 1].y);
      if (len > max_len) {
        pts_.erase(pts_.begin(), pts_.begin() + (i - 1));
        return;
      }
    }
  }

  const std::deque<Pt>& points() const { return pts_; }
  float x() const { return x_; }
  float y() const { return y_; }

 private:
  std::deque<Pt> pts_;
  float x_ = 0, y_ = 0;
};

// Position of `v` inside a pad of `size`, starting from the centre and wrapping at the edges.
inline float WrapInPad(float v, float size) {
  float r = std::fmod(v + size / 2, size);
  return r < 0 ? r + size : r;
}
