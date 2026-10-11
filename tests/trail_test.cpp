// Mouse trail: merging fast samples, trimming by age and length, wrapping into the pad.
#undef NDEBUG  // asserts are the test
#include <cassert>
#include <cstdio>

#include "../src/trail.h"

int main() {
  {  // samples within the merge window move the newest point instead of adding one
    MouseTrail t;
    t.Move(1, 0, 0, false, 4);
    t.Move(1, 0, 2, true, 4);
    assert(t.points().size() == 1 && t.points().back().x == 2 && t.points().back().m1);
    t.Move(1, 0, 10, false, 4);
    assert(t.points().size() == 2 && t.x() == 3);
  }
  {  // old points fade out
    MouseTrail t;
    for (int i = 0; i < 10; i++) t.Move(0.1f, 0, i * 100, false, 4);
    t.Trim(1000, 500, 100);  // keeps t = 500..900
    assert(t.points().size() == 5 && t.points().front().t == 500);
  }
  {  // length cap keeps the newest stretch, ending on the point just past the cap
    MouseTrail t;
    for (int i = 0; i < 10; i++) t.Move(1, 0, i * 10, false, 4);  // x = 1..10
    t.Trim(100, 1000, 3.5f);
    assert(t.points().size() == 5 && t.points().front().x == 6 && t.points().back().x == 10);
  }
  {  // wrapping: 0 is the pad centre, edges wrap around
    assert(WrapInPad(0, 4) == 2);
    assert(WrapInPad(1, 4) == 3);
    assert(WrapInPad(2.5f, 4) == 0.5f);
    assert(WrapInPad(-2.5f, 4) == 3.5f);
  }
  std::puts("trail ok");
}
