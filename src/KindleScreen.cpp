#include "KindleScreen.h"

#include <fbink.h>

#include "EInkDisplay.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace kindle_screen {
namespace {

std::mutex screenMutex;
int fbfd = -1;
FBInkConfig cfg{};
FBInkState state{};
uint8_t* fbMem = nullptr;
size_t fbSize = 0;

uint16_t bufW = 0;  // firmware buffer (landscape); smaller than the panel when zoomed
uint16_t bufH = 0;
uint16_t panW = 0;  // panel (portrait), from FBInk: 1072 on the PW3, 1860 on the Scribe
uint16_t panH = 0;  //                                1448 on the PW3, 2480 on the Scribe

// Bilinear sample positions for zoomed frames, computed once in begin().
// Panel row v reads buffer column x; panel column u reads buffer row y.
struct Tap {
  uint16_t i0, i1;  // neighbouring buffer indices
  uint16_t w1;      // weight of i1, 0..256
};
std::vector<Tap> rowTaps;  // per panel row v  -> buffer x
std::vector<Tap> colTaps;  // per panel column u -> buffer y
bool scaled = false;

Tap makeTap(int outIndex, int outSize, int inSize) {
  // Pixel-centre mapping: in = (out + 0.5) * in/out - 0.5
  const float f = (outIndex + 0.5f) * static_cast<float>(inSize) / outSize - 0.5f;
  const float c = std::clamp(f, 0.0f, static_cast<float>(inSize - 1));
  const int i0 = static_cast<int>(c);
  const int i1 = std::min(i0 + 1, inSize - 1);
  return {static_cast<uint16_t>(i0), static_cast<uint16_t>(i1),
          static_cast<uint16_t>((c - i0) * 256.0f + 0.5f)};
}

std::vector<uint8_t> panel;      // frame being built
std::vector<uint8_t> onScreen;   // what we last sent to the EPDC
bool haveOnScreen = false;
WFM_MODE_INDEX_T fastWaveform = WFM_GL16;

// Test mode: CPK_FAKE_FB=<dir> writes each refresh as <dir>/frame-NNNN.pgm
// instead of driving /dev/fb0 (for running under qemu-arm on a PC).
std::string fakeDir;
std::vector<uint8_t> fakeMem;
int fakeFrame = 0;

WFM_MODE_INDEX_T parseWaveform(const char* name, WFM_MODE_INDEX_T fallback) {
  if (!name || !*name) return fallback;
  const std::string n(name);
  if (n == "AUTO") return WFM_AUTO;
  if (n == "DU") return WFM_DU;
  if (n == "A2") return WFM_A2;
  if (n == "GL16") return WFM_GL16;
  if (n == "GC16") return WFM_GC16;
  if (n == "REAGL") return WFM_REAGL;
  std::fprintf(stderr, "[kindle] unknown CPK_FAST_WAVEFORM '%s', using GL16\n", name);
  return fallback;
}

// Build the portrait panel image from the landscape firmware buffer.
void buildPanel(const uint8_t* gray, Mapping mapping) {
  uint8_t* out = panel.data();
  if (scaled) {
    // Zoomed: P(u, v) = B(x = v * bufW / panH, y = (panW - 1 - u) * bufH / panW),
    // bilinear. (Only RotateCW is used; see HalDisplay panelMapping.)
    for (uint16_t v = 0; v < panH; ++v) {
      const Tap& tx = rowTaps[v];
      const uint32_t wx1 = tx.w1, wx0 = 256 - wx1;
      uint8_t* row = out + static_cast<size_t>(v) * panW;
      for (uint16_t u = 0; u < panW; ++u) {
        const Tap& ty = colTaps[panW - 1 - u];
        const uint8_t* r0 = gray + static_cast<size_t>(ty.i0) * bufW;
        const uint8_t* r1 = gray + static_cast<size_t>(ty.i1) * bufW;
        const uint32_t top = r0[tx.i0] * wx0 + r0[tx.i1] * wx1;
        const uint32_t bot = r1[tx.i0] * wx0 + r1[tx.i1] * wx1;
        row[u] = static_cast<uint8_t>((top * (256 - ty.w1) + bot * ty.w1 + (1u << 15)) >> 16);
      }
    }
    return;
  }
  if (mapping == Mapping::RotateCW) {
    // P(u, v) = B(x = v, y = H - 1 - u)
    for (uint16_t v = 0; v < panH; ++v) {
      const uint8_t* col = gray + v;  // buffer column x = v
      uint8_t* row = out + static_cast<size_t>(v) * panW;
      for (uint16_t u = 0; u < panW; ++u)
        row[u] = col[static_cast<size_t>(bufH - 1 - u) * bufW];
    }
  } else {
    // P(u, v) = B(x = W - 1 - v, y = u)
    for (uint16_t v = 0; v < panH; ++v) {
      const uint8_t* col = gray + (bufW - 1 - v);
      uint8_t* row = out + static_cast<size_t>(v) * panW;
      for (uint16_t u = 0; u < panW; ++u)
        row[u] = col[static_cast<size_t>(u) * bufW];
    }
  }
}

struct Rect {
  uint16_t left, top, width, height;
};

bool dirtyRect(Rect& r) {
  if (!haveOnScreen) {
    r = {0, 0, panW, panH};
    return true;
  }
  int top = -1, bottom = -1, left = panW, right = -1;
  for (uint16_t y = 0; y < panH; ++y) {
    const uint8_t* a = panel.data() + static_cast<size_t>(y) * panW;
    const uint8_t* b = onScreen.data() + static_cast<size_t>(y) * panW;
    if (std::memcmp(a, b, panW) == 0) continue;
    if (top < 0) top = y;
    bottom = y;
    int l = 0;
    while (a[l] == b[l]) ++l;
    int rr = panW - 1;
    while (a[rr] == b[rr]) --rr;
    left = std::min(left, l);
    right = std::max(right, rr);
  }
  if (top < 0) return false;
  r = {static_cast<uint16_t>(left), static_cast<uint16_t>(top),
       static_cast<uint16_t>(right - left + 1),
       static_cast<uint16_t>(bottom - top + 1)};
  return true;
}

void copyToFramebuffer(const Rect& r) {
  const bool invert = state.inverted_grayscale;
  for (uint16_t y = r.top; y < r.top + r.height; ++y) {
    const uint8_t* src = panel.data() + static_cast<size_t>(y) * panW + r.left;
    uint8_t* dst = fbMem + static_cast<size_t>(y) * state.scanline_stride;
    if (state.bpp == 8) {
      dst += r.left;
      if (invert) {
        for (uint16_t i = 0; i < r.width; ++i) dst[i] = static_cast<uint8_t>(~src[i]);
      } else {
        std::memcpy(dst, src, r.width);
      }
    } else if (state.bpp == 32) {
      uint32_t* px = reinterpret_cast<uint32_t*>(dst) + r.left;
      for (uint16_t i = 0; i < r.width; ++i) {
        const uint32_t g = src[i];
        px[i] = 0xFF000000u | (g << 16) | (g << 8) | g;
      }
    } else if (state.bpp == 16) {
      uint16_t* px = reinterpret_cast<uint16_t*>(dst) + r.left;
      for (uint16_t i = 0; i < r.width; ++i) {
        const uint16_t g = src[i];
        px[i] = static_cast<uint16_t>(((g >> 3) << 11) | ((g >> 2) << 5) | (g >> 3));
      }
    }
  }
}

void sendRefresh(const Rect& r, Waveform waveform) {
  FBInkConfig c = cfg;
  switch (waveform) {
    case Waveform::Fast:
      c.wfm_mode = fastWaveform;
      c.is_flashing = false;
      break;
    case Waveform::Quality:
      c.wfm_mode = WFM_GC16;
      c.is_flashing = false;
      break;
    case Waveform::Full:
      c.wfm_mode = WFM_GC16;
      c.is_flashing = true;
      break;
  }
  if (!fakeDir.empty()) {
    char path[512];
    std::snprintf(path, sizeof(path), "%s/frame-%04d.pgm", fakeDir.c_str(), fakeFrame++);
    if (FILE* f = std::fopen(path, "wb")) {
      std::fprintf(f, "P5\n%u %u\n255\n", panW, panH);
      std::fwrite(fakeMem.data(), 1, fakeMem.size(), f);
      std::fclose(f);
    }
    std::fprintf(stderr, "[kindle] refresh %s wfm=%d flash=%d rect=%u,%u %ux%u\n", path,
                 static_cast<int>(c.wfm_mode), c.is_flashing ? 1 : 0, r.left, r.top, r.width,
                 r.height);
    return;
  }
  const int rc = fbink_refresh(fbfd, r.top, r.left, r.width, r.height, &c);
  if (rc < 0) std::fprintf(stderr, "[kindle] fbink_refresh failed: %d\n", rc);
}

// Kindle: the panel is found once, before anything is sized from it:
// kindle_geometry::get() needs it to size the firmware's buffer, and begin()
// then drives it. Fake mode (CPK_FAKE_FB) takes the size from CPK_FAKE_PANEL
// ("WIDTHxHEIGHT", portrait; default the PW3's 1072x1448).
bool openPanel() {
  if (const char* fake = std::getenv("CPK_FAKE_FB"); fake && *fake) {
    fakeDir = fake;
    unsigned w = 1072, h = 1448;
    if (const char* p = std::getenv("CPK_FAKE_PANEL"); p && *p &&
        std::sscanf(p, "%ux%u", &w, &h) != 2) {
      std::fprintf(stderr, "[kindle] bad CPK_FAKE_PANEL '%s', using 1072x1448\n", p);
      w = 1072;
      h = 1448;
    }
    state = FBInkState{};
    state.screen_width = w;
    state.screen_height = h;
    state.scanline_stride = w;
    state.bpp = 8;
    std::snprintf(state.device_name, sizeof(state.device_name), "fake");
    fakeMem.assign(static_cast<size_t>(w) * h, 0xFF);
    fbMem = fakeMem.data();
    fbSize = fakeMem.size();
    std::fprintf(stderr, "[kindle] fake framebuffer %ux%u -> %s\n", w, h, fakeDir.c_str());
  } else {
    fbfd = fbink_open();
    if (fbfd < 0) {
      std::fprintf(stderr, "[kindle] cannot open framebuffer\n");
      return false;
    }
    cfg = FBInkConfig{};
    cfg.is_quiet = true;
    if (fbink_init(fbfd, &cfg) < 0) {
      std::fprintf(stderr, "[kindle] fbink_init failed\n");
      return false;
    }
    fbink_get_state(&cfg, &state);
    fbMem = fbink_get_fb_pointer(fbfd, &fbSize);
    std::fprintf(stderr,
                 "[kindle] device=%s (%s) fb=%ux%u stride=%u bpp=%u rota=%u inverted=%d\n",
                 state.device_name, state.device_codename, state.screen_width,
                 state.screen_height, state.scanline_stride, state.bpp, state.current_rota,
                 state.inverted_grayscale ? 1 : 0);
    if (!fbMem) {
      std::fprintf(stderr, "[kindle] no framebuffer mapping\n");
      return false;
    }
  }
  // The firmware's buffers are sized for the largest supported panel
  // (EInkDisplay::DISPLAY_*, landscape), and the panel must be portrait.
  if (state.screen_width > state.screen_height) {
    std::fprintf(stderr,
                 "[kindle] panel is %ux%u (landscape); start from the Kindle home screen "
                 "in portrait orientation\n",
                 state.screen_width, state.screen_height);
    fbMem = nullptr;
    return false;
  }
  if (state.screen_width > EInkDisplay::DISPLAY_HEIGHT ||
      state.screen_height > EInkDisplay::DISPLAY_WIDTH || state.screen_width < 400 ||
      state.screen_height < 400) {
    std::fprintf(stderr, "[kindle] panel %ux%u is not supported (largest: %ux%u)\n",
                 state.screen_width, state.screen_height, EInkDisplay::DISPLAY_HEIGHT,
                 EInkDisplay::DISPLAY_WIDTH);
    fbMem = nullptr;
    return false;
  }
  if (state.bpp != 8 && state.bpp != 16 && state.bpp != 32) {
    std::fprintf(stderr, "[kindle] unsupported bit depth %u\n", state.bpp);
    fbMem = nullptr;
    return false;
  }
  panW = static_cast<uint16_t>(state.screen_width);
  panH = static_cast<uint16_t>(state.screen_height);
  return true;
}

bool panelReady() {
  static const bool ok = openPanel();
  return ok;
}

}  // namespace

bool panelSize(uint16_t& portraitWidth, uint16_t& portraitHeight) {
  if (!panelReady()) return false;
  portraitWidth = panW;
  portraitHeight = panH;
  return true;
}

bool begin(uint16_t bufferWidth, uint16_t bufferHeight) {
  if (!panelReady()) return false;
  std::lock_guard<std::mutex> lock(screenMutex);
  bufW = bufferWidth;
  bufH = bufferHeight;
  scaled = bufW != panH || bufH != panW;
  if (scaled) {
    rowTaps.resize(panH);
    colTaps.resize(panW);
    for (int v = 0; v < panH; ++v) rowTaps[v] = makeTap(v, panH, bufW);
    for (int u = 0; u < panW; ++u) colTaps[u] = makeTap(u, panW, bufH);
    std::fprintf(stderr, "[kindle] zoom: drawing %ux%u, scaled to %ux%u\n", bufW, bufH, panH, panW);
  }
  panel.assign(static_cast<size_t>(panW) * panH, 0xFF);
  onScreen.assign(panel.size(), 0xFF);
  fastWaveform = parseWaveform(std::getenv("CPK_FAST_WAVEFORM"), WFM_GL16);
  return true;
}

void present(const uint8_t* gray, Mapping mapping, Waveform waveform) {
  std::lock_guard<std::mutex> lock(screenMutex);
  if (!fbMem) return;
  buildPanel(gray, mapping);
  // First frame: flash the whole panel so nothing of the Kindle UI survives.
  if (!haveOnScreen) waveform = Waveform::Full;
  Rect r{};
  if (waveform == Waveform::Full) {
    r = {0, 0, panW, panH};
  } else if (!dirtyRect(r)) {
    return;
  }
  copyToFramebuffer(r);
  sendRefresh(r, waveform);
  // buildPanel() rewrites every pixel, so a swap is enough here.
  onScreen.swap(panel);
  haveOnScreen = true;
}

void redrawLast() {
  std::lock_guard<std::mutex> lock(screenMutex);
  if (!fbMem || !haveOnScreen) return;
  panel = onScreen;
  const Rect r{0, 0, panW, panH};
  copyToFramebuffer(r);
  sendRefresh(r, Waveform::Full);
}

void touchQuirks(bool& swapAxes, bool& mirrorX, bool& mirrorY) {
  swapAxes = state.touch_swap_axes;
  mirrorX = state.touch_mirror_x;
  mirrorY = state.touch_mirror_y;
}

}  // namespace kindle_screen
