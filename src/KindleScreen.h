#pragma once
// Kindle e-ink output for the CrossPoint host build.
//
// The firmware draws into a landscape buffer: the panel (1448 x 1072 on the
// PW3, 2480 x 1860 on the Scribe) divided by CPK_ZOOM, so 1152 x 853 on the
// PW3 at the default 125% (KindleGeometry.h).
// KindleScreen scales that up to the panel (bilinear), rotates it into the
// portrait panel image, finds the rectangle that changed since the last
// update, copies only that rectangle into the Linux framebuffer and asks the
// EPDC (via FBInk) to refresh it.

#include <cstdint>

namespace kindle_screen {

enum class Waveform : uint8_t {
  Fast,    // page turns and UI changes (default GL16, see CPK_FAST_WAVEFORM)
  Quality, // GC16 partial: clean grays, no flash
  Full,    // GC16 with a black flash: clears ghosting
};

// How the landscape buffer maps onto the portrait panel.
enum class Mapping : uint8_t {
  RotateCW,  // Portrait and LandscapeCounterClockwise
  RotateCCW, // PortraitInverted and LandscapeClockwise
};

// The panel's portrait size from FBInk (opens /dev/fb0 on first use; fake
// mode: CPK_FAKE_PANEL). False if the panel cannot be driven.
bool panelSize(uint16_t& portraitWidth, uint16_t& portraitHeight);

// Prepares drawing for a firmware buffer of this size (landscape). Returns
// false if the panel cannot be driven (the caller exits with code 3).
bool begin(uint16_t bufferWidth, uint16_t bufferHeight);

// Present a frame. `gray` is bufferWidth*bufferHeight bytes, 0 = black,
// 255 = white, in firmware buffer coordinates. Thread-safe.
void present(const uint8_t* gray, Mapping mapping, Waveform waveform);

// Push the last frame again with a full flash (used after the Kindle wakes,
// because powerd/blanket may have drawn over the panel meanwhile).
void redrawLast();

// Touch axis quirks FBInk knows for this Kindle model.
void touchQuirks(bool& swapAxes, bool& mirrorX, bool& mirrorY);

}  // namespace kindle_screen
