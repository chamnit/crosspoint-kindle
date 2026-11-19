#pragma once
// Runtime size of the firmware's drawing buffer on the Kindle.
//
// The panel size comes from FBInk at startup (KindleScreen panelSize): 1448x1072
// on the Paperwhite 3, 2480x1860 on the Scribe (landscape buffer terms), both
// 300 ppi. CrossPoint's UI is laid out for ~220 ppi screens, so at 1:1
// everything is small. CPK_ZOOM (percent, 100-200, default 125) makes the
// firmware draw into a smaller buffer that KindleScreen scales up to the full
// panel. Touch input is normalized, so it follows automatically.
// EInkDisplay::DISPLAY_* are only the largest supported panel, for storage.

#include <cstdint>
#include <cstdlib>

#include "EInkDisplay.h"
#include "KindleScreen.h"

namespace kindle_geometry {

struct Geometry {
  uint16_t width;       // firmware buffer, landscape, multiple of 8
  uint16_t height;
  uint16_t widthBytes;  // 1-bit framebuffer stride
  uint32_t bufferSize;  // 1-bit framebuffer bytes
  uint16_t zoomPercent;
};

inline const Geometry& get() {
  static const Geometry g = [] {
    // Landscape panel size. If the panel cannot be opened, HalDisplay::begin
    // exits (code 3); the PW3 size only keeps the numbers sane until then.
    uint16_t portraitW = 1072, portraitH = 1448;
    kindle_screen::panelSize(portraitW, portraitH);
    const long kPanelW = portraitH;
    const long kPanelH = portraitW;
    long zoom = 125;
    if (const char* z = std::getenv("CPK_ZOOM"); z && *z) zoom = std::strtol(z, nullptr, 10);
    if (zoom < 100) zoom = 100;
    if (zoom > 200) zoom = 200;
    Geometry out{};
    out.zoomPercent = static_cast<uint16_t>(zoom);
    out.width = static_cast<uint16_t>((kPanelW * 100 / zoom) & ~7L);
    out.height = static_cast<uint16_t>((kPanelH * out.width + kPanelW / 2) / kPanelW);
    if (zoom == 100) {
      out.width = static_cast<uint16_t>(kPanelW & ~7L);
      out.height = static_cast<uint16_t>(kPanelH);
    }
    out.widthBytes = out.width / 8;
    out.bufferSize = static_cast<uint32_t>(out.widthBytes) * out.height;
    return out;
  }();
  return g;
}

}  // namespace kindle_geometry
