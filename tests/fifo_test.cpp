// Timeline placement of captured audio: contiguous, jittered, gapped, overlapping and late packets.
#undef NDEBUG  // asserts are the test
#include <cassert>
#include <cstdio>

#include "../src/fifo.h"

static std::vector<float> Ramp(int64_t from, uint32_t frames) {  // sample value = its timeline position
  std::vector<float> v;
  for (uint32_t i = 0; i < frames; i++) v.insert(v.end(), 2, float(from + i));
  return v;
}

static void Expect(SampleFifo& f, int64_t at, auto&& want) {  // want(pos) -> expected value
  std::vector<float> out(kFrame * 2);
  f.Read(at, out.data());
  for (int i = 0; i < kFrame; i++) {
    if (out[2 * i] != want(at + i) || out[2 * i + 1] != want(at + i)) {
      std::printf("at %lld: got %g want %g\n", (long long)(at + i), out[2 * i], want(at + i));
      assert(false);
    }
  }
}

int main() {
  auto same = [](int64_t p) { return float(p); };
  {  // contiguous packets of 480 come back exactly
    SampleFifo f;
    for (int64_t p = 0; p < 4800; p += 480) f.Write(p, Ramp(p, 480).data(), 480);
    Expect(f, 0, same);
    Expect(f, kFrame, same);
  }
  {  // a packet stamped 5 ms late is treated as contiguous, not padded
    SampleFifo f;
    f.Write(0, Ramp(0, 480).data(), 480);
    f.Write(480 + 240, Ramp(480, 480).data(), 480);
    Expect(f, 0, [](int64_t p) { return p < 960 ? float(p) : 0.f; });
  }
  {  // a 100 ms gap becomes silence, data after it lands at its timestamp
    SampleFifo f;
    f.Write(0, Ramp(0, 480).data(), 480);
    f.Write(5280, Ramp(5280, 2048).data(), 2048);
    Expect(f, 0, [](int64_t p) { return p < 480 ? float(p) : 0.f; });
    Expect(f, 5280, same);
  }
  {  // overlapping packet: the already-placed part is dropped
    SampleFifo f;
    f.Write(0, Ramp(0, 2048).data(), 2048);
    f.Write(1000, Ramp(1000, 2048).data(), 2048);  // re-sends 1000..2047, adds 2048..3047
    f.Write(3048, Ramp(3048, 1024).data(), 1024);
    Expect(f, 0, same);
    Expect(f, kFrame, same);
    Expect(f, 2 * kFrame, same);
  }
  {  // reader ahead of writer: silence, then the late data that's still in range
    SampleFifo f;
    f.Write(0, Ramp(0, 512).data(), 512);
    Expect(f, 0, [](int64_t p) { return p < 512 ? float(p) : 0.f; });
    Expect(f, kFrame, [](int64_t) { return 0.f; });
    f.Write(2 * kFrame + 1000, Ramp(2 * kFrame + 1000, 1024).data(), 1024);  // beyond jitter: a real gap
    Expect(f, 2 * kFrame, [](int64_t p) { return p >= 2 * kFrame + 1000 ? float(p) : 0.f; });
  }
  {  // silent packets (nullptr) are zeros and keep the timeline moving
    SampleFifo f;
    f.Write(0, nullptr, 1024);
    f.Write(1024, Ramp(1024, 1024).data(), 1024);
    Expect(f, 0, [](int64_t) { return 0.f; });
    Expect(f, kFrame, same);
  }
  std::puts("fifo ok");
}
