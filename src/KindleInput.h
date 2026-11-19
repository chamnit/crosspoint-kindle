#pragma once
// Kindle touchscreen input for the CrossPoint host build.
//
// Reads the multitouch evdev device directly (protocol A or B, or single-touch
// ABS_X/ABS_Y) and turns it into a small set of events in portrait panel
// coordinates normalized to 0..1. Gestures that have no hardware button on the
// Paperwhite are synthesized here:
//   two-finger tap        -> Home
// (Leaving CrossPoint is Home > "Exit CrossPoint...", which asks first.)

#include <vector>

namespace kindle_input {

struct Event {
  enum class Type { Down, Move, Up, Cancel, HomeTap, PowerDown, PowerUp } type;
  float u = 0.0f;  // 0 = left edge of the panel held upright
  float v = 0.0f;  // 0 = top edge
};

struct Orientation {
  bool swapAxes = false;
  bool mirrorX = false;
  bool mirrorY = false;
};

// Finds and grabs the touchscreen. `defaults` come from FBInk's device quirks;
// CPK_TOUCH_SWAP_XY / CPK_TOUCH_MIRROR_X / CPK_TOUCH_MIRROR_Y (0 or 1)
// override them.
bool begin(Orientation defaults);
// Non-blocking. Appends any events since the last call.
void poll(std::vector<Event>& out);

// Waits up to timeoutMs for a power key press (used while asleep). Returns
// true on a press. Other input is discarded.
bool waitForPowerPress(int timeoutMs);
// Blocks until the touchscreen or power key has an event to read, or
// timeoutMs pass (a signal also ends the wait). Events are left queued.
void waitForInput(int timeoutMs);
// Releases the grab so the Kindle UI gets touches again.
void end();

}  // namespace kindle_input
