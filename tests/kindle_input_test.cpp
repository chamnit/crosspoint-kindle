// Host test for the evdev gesture parser: feeds synthetic multitouch
// protocol-B streams into KindleInput.cpp and checks the emitted events.
// Build: g++ -std=c++17 -I../src kindle_input_test.cpp -o /tmp/kit && /tmp/kit
#include "../src/KindleInput.cpp"

#include <cassert>
#include <cstdio>
#include <thread>

using namespace kindle_input;
using T = Event::Type;

static std::vector<Event> out;
static void ev(int type, int code, int value) {
  input_event e{};
  e.type = type;
  e.code = code;
  e.value = value;
  handle(e, out);
}
static void syn() { ev(EV_SYN, SYN_REPORT, 0); }
static void down(int slot, int id, int x, int y) {
  ev(EV_ABS, ABS_MT_SLOT, slot);
  ev(EV_ABS, ABS_MT_TRACKING_ID, id);
  ev(EV_ABS, ABS_MT_POSITION_X, x);
  ev(EV_ABS, ABS_MT_POSITION_Y, y);
}
static void move(int slot, int x, int y) {
  ev(EV_ABS, ABS_MT_SLOT, slot);
  ev(EV_ABS, ABS_MT_POSITION_X, x);
  ev(EV_ABS, ABS_MT_POSITION_Y, y);
}
static void up(int slot) {
  ev(EV_ABS, ABS_MT_SLOT, slot);
  ev(EV_ABS, ABS_MT_TRACKING_ID, -1);
}
static void reset() {
  out.clear();
  for (Slot& s : slots) s = Slot{};
  prevCount = 0;
  primarySlot = -1;
  multi = false;
}

int main() {
  absX.minimum = 0; absX.maximum = 1071;
  absY.minimum = 0; absY.maximum = 1447;
  multiTouch = true;

  // 1. Single tap -> Down, Up at the same spot.
  reset();
  down(0, 7, 535, 724); syn();
  up(0); syn();
  assert(out.size() == 2 && out[0].type == T::Down && out[1].type == T::Up);
  assert(std::fabs(out[0].u - 0.5f) < 0.01f && std::fabs(out[0].v - 0.5f) < 0.01f);
  std::puts("tap ok");

  // 2. Swipe -> Down, Moves, Up at the end point.
  reset();
  down(0, 8, 900, 700); syn();
  for (int x = 800; x >= 200; x -= 100) { move(0, x, 700); syn(); }
  up(0); syn();
  assert(out.front().type == T::Down && out.back().type == T::Up);
  assert(out.back().u < 0.2f);
  std::puts("swipe ok");

  // 3. Two-finger tap -> Down, Cancel, HomeTap (no page tap).
  reset();
  down(0, 9, 300, 700); syn();
  down(1, 10, 700, 700); syn();
  up(0); up(1); syn();
  assert(out.size() == 3 && out[0].type == T::Down && out[1].type == T::Cancel &&
         out[2].type == T::HomeTap);
  std::puts("two-finger tap ok");

  // 3b. Both fingers in the same frame -> HomeTap only.
  reset();
  down(0, 11, 300, 700); down(1, 12, 700, 700); syn();
  up(0); up(1); syn();
  assert(out.size() == 1 && out[0].type == T::HomeTap);
  std::puts("simultaneous two-finger tap ok");

  // 4. A long two-finger hold does nothing (no Home, no exit).
  reset();
  down(0, 13, 300, 700); down(1, 14, 700, 700); syn();
  multiStartMs = nowMs() - kHomeTapMaxMs - 10;  // held longer than a tap
  up(0); up(1); syn();
  assert(out.empty());
  std::puts("two-finger hold ok");

  // 5. Mirror/swap transforms.
  reset();
  orient = {true, true, false};
  down(0, 15, 0, 1447); syn();  // raw x=0,y=max -> swap -> (1,0) -> mirrorX -> (0,0)
  assert(out[0].u < 0.01f && out[0].v < 0.01f);
  std::puts("transform ok");

  // 6. SYN_DROPPED mid-touch -> Cancel, state cleared.
  reset();
  orient = {};
  down(0, 16, 100, 100); syn();
  ev(EV_SYN, SYN_DROPPED, 0);
  assert(out.back().type == T::Cancel && prevCount == 0);
  std::puts("syn_dropped ok");
  std::puts("ALL PASS");
}
