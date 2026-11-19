// Display for the Kindle build. Originally the simulator's SDL window; here
// the firmware still draws into its 1-bit B/W and 2-bit grayscale buffers as
// on the device, and this file turns them into one 8-bit gray frame
// (pixelBuf) that KindleScreen.cpp scales by CPK_ZOOM, rotates and sends to
// the e-ink panel through FBInk. Kindle changes are marked "Kindle:".
#include "HalDisplay.h"
#include "KindleGeometry.h"
#include "SimScreenText.h"

#include <BoardConfig.h>
#include <GfxRenderer.h>
#include "KindleScreen.h"

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

// Frame written by the render task and handed to the panel.
// Kindle: 8-bit gray, 0 = black, 255 = white, firmware buffer coordinates.
// Buffer offsets must not be uint16: the zoomed buffer is over 64 KiB (the
// simulator's uint16 offsets garbled images; see drawImage).
static uint8_t
    pixelBuf[HalDisplay::DISPLAY_WIDTH * HalDisplay::DISPLAY_HEIGHT];
static std::mutex pixelBufMutex;
static std::atomic<bool> pendingPresent{false};
// Set by HalGPIO (exit gesture, Exit menu item, scripted QUIT) and the signal
// handlers; read by shouldQuit().
std::atomic<bool> quitRequested{false};


namespace {

struct GrayscalePreviewState {
  std::array<uint8_t, HalDisplay::BUFFER_SIZE> bwBase{};
  std::array<uint8_t, HalDisplay::BUFFER_SIZE> lsbPlane{};
  std::array<uint8_t, HalDisplay::BUFFER_SIZE> msbPlane{};
  bool bwBaseValid = false;
  bool lsbValid = false;
  bool msbValid = false;
  bool absolute = false;
};

constexpr uint8_t kGrayWhite = 255;
constexpr uint8_t kGrayLight = 200;
constexpr uint8_t kGrayDark = 96;
constexpr uint8_t kGrayBlack = 0;

GrayscalePreviewState grayscalePreviewState;
std::array<uint8_t, HalDisplay::BUFFER_SIZE> frameBufferStorage{};
bool frameBufferLent = false;

struct ScreenshotEvent {
  unsigned long atMs;
  std::string path;
  bool handled = false;
};

// Script SHOT actions add to the schedule from the input path.
std::mutex screenshotMutex;
std::vector<ScreenshotEvent> screenshotEvents;
bool screenshotEventsInitialized = false;
const std::thread::id simulatorMainThread = std::this_thread::get_id();

void initializeScreenshotEvents() {
  if (screenshotEventsInitialized)
    return;
  screenshotEventsInitialized = true;

  const char *schedule = std::getenv("CROSSPOINT_SIM_SCREENSHOTS");
  if (!schedule || schedule[0] == '\0')
    return;

  const std::string spec(schedule);
  size_t start = 0;
  while (start < spec.size()) {
    const size_t end = spec.find(';', start);
    const std::string item = spec.substr(
        start, end == std::string::npos ? std::string::npos : end - start);
    const size_t colon = item.find(':');
    if (colon != std::string::npos && colon + 1 < item.size()) {
      screenshotEvents.push_back(
          {std::strtoul(item.substr(0, colon).c_str(), nullptr, 10),
           item.substr(colon + 1)});
    }
    if (end == std::string::npos)
      break;
    start = end + 1;
  }
}

bool hasDueScreenshot() {
  const std::lock_guard<std::mutex> lock(screenshotMutex);
  initializeScreenshotEvents();
  const unsigned long now = millis();
  for (const auto &event : screenshotEvents) {
    if (!event.handled && event.atMs <= now)
      return true;
  }
  return false;
}

bool saveRendererBmp(const std::string &path) {
  std::cerr << "[kindle] screenshots are not supported: " << path << std::endl;
  return false;
}

void captureDueScreenshots() {
  const std::lock_guard<std::mutex> lock(screenshotMutex);
  const unsigned long now = millis();
  for (auto &event : screenshotEvents) {
    if (event.handled || event.atMs > now)
      continue;
    event.handled = true;
    saveRendererBmp(event.path);
  }
}

uint8_t argbGray(uint8_t level) { return level; }

bool getBit(const uint8_t *buffer, int x, int y) {
  const int byteIdx = (y * kindle_geometry::get().width + x) / 8;
  const int bitIdx = 7 - (x % 8);
  return (buffer[byteIdx] & (1 << bitIdx)) != 0;
}

void renderBwPixels(const uint8_t *fb) {
  const std::lock_guard<std::mutex> lock(pixelBufMutex);
  const bool invert = display.isInverted();
  for (int y = 0; y < kindle_geometry::get().height; y++) {
    for (int x = 0; x < kindle_geometry::get().width; x++) {
      const bool white = getBit(fb, x, y);
      pixelBuf[y * kindle_geometry::get().width + x] =
          (white != invert) ? 255 : 0;
    }
  }
  pendingPresent.store(true);
}

void clearGrayscalePlanes() {
  grayscalePreviewState.lsbPlane.fill(0);
  grayscalePreviewState.msbPlane.fill(0);
  grayscalePreviewState.lsbValid = false;
  grayscalePreviewState.msbValid = false;
}

void snapshotBwBase(const uint8_t *fb) {
  memcpy(grayscalePreviewState.bwBase.data(), fb, kindle_geometry::get().bufferSize);
  grayscalePreviewState.bwBaseValid = true;
  clearGrayscalePlanes();
}

void copyPlane(std::array<uint8_t, HalDisplay::BUFFER_SIZE> &dst,
               const uint8_t *src, bool &valid) {
  if (!src) {
    valid = false;
    dst.fill(0);
    return;
  }
  memcpy(dst.data(), src, kindle_geometry::get().bufferSize);
  valid = true;
}

void composeGrayscalePreview() {
  const std::lock_guard<std::mutex> lock(pixelBufMutex);
  const uint8_t *bwBase = grayscalePreviewState.bwBaseValid
                              ? grayscalePreviewState.bwBase.data()
                              : display.getFrameBuffer();
  for (int y = 0; y < kindle_geometry::get().height; y++) {
    for (int x = 0; x < kindle_geometry::get().width; x++) {
      const bool baseWhite = getBit(bwBase, x, y);
      const bool lsbActive =
          grayscalePreviewState.lsbValid &&
          getBit(grayscalePreviewState.lsbPlane.data(), x, y);
      const bool msbActive =
          grayscalePreviewState.msbValid &&
          getBit(grayscalePreviewState.msbPlane.data(), x, y);

      uint8_t level = kGrayWhite;
      if (grayscalePreviewState.absolute) {
        const uint8_t value = (lsbActive ? 1 : 0) | (msbActive ? 2 : 0);
        switch (value) {
          case 0:
            level = kGrayBlack;
            break;
          case 1:
            level = kGrayDark;
            break;
          case 2:
            level = kGrayLight;
            break;
          default:
            level = kGrayWhite;
            break;
        }
      } else if (!baseWhite) {
        if (msbActive) {
          level = lsbActive ? kGrayDark : kGrayLight;
        } else if (lsbActive) {
          level = kGrayDark;
        } else {
          level = kGrayBlack;
        }
      }

      if (display.isInverted())
        level = static_cast<uint8_t>(255 - level);
      pixelBuf[y * kindle_geometry::get().width + x] = argbGray(level);
    }
  }
  pendingPresent.store(true);
}

} // namespace

void scheduleSimulatorScreenshot(const std::string &path) {
  const std::lock_guard<std::mutex> lock(screenshotMutex);
  initializeScreenshotEvents();
  screenshotEvents.push_back({millis(), path});
}

static bool isPortraitOrientation(GfxRenderer::Orientation orientation) {
  return orientation == GfxRenderer::Portrait ||
         orientation == GfxRenderer::PortraitInverted;
}

// The firmware buffer is landscape and the renderer rotates content for each
// orientation itself, exactly as on a device whose panel is fixed in the case.
// So the buffer-to-panel mapping is fixed too: the buffer's left edge is the
// top of the Paperwhite held upright. HalGPIO's panelToLogical() is its inverse.
static kindle_screen::Mapping panelMapping(GfxRenderer::Orientation) {
  return kindle_screen::Mapping::RotateCW;
}

static kindle_screen::Waveform waveformFor(HalDisplay::RefreshMode mode) {
  return mode == HalDisplay::FULL_REFRESH   ? kindle_screen::Waveform::Full
         : mode == HalDisplay::HALF_REFRESH ? kindle_screen::Waveform::Quality
                                            : kindle_screen::Waveform::Fast;
}

// The Kindle EPDC shows 16 grays in one update, so the firmware's B/W base and
// its gray planes go out together ("combined base", like Paper Mono). The base
// call only records which refresh it asked for; the gray pass presents.
static bool basePending = false;
static HalDisplay::RefreshMode pendingBaseMode = HalDisplay::FAST_REFRESH;

static void presentToPanel(kindle_screen::Waveform waveform) {
  extern GfxRenderer renderer;
  const std::lock_guard<std::mutex> lock(pixelBufMutex);
  kindle_screen::present(pixelBuf, panelMapping(renderer.getOrientation()),
                         waveform);
}

HalDisplay::HalDisplay() {}
HalDisplay::~HalDisplay() {}

const char* HalDisplay::controllerName() const {
  switch (BoardConfig::ACTIVE.displayController) {
    case BoardConfig::DisplayController::UC8253:
      return "UC8253";
    case BoardConfig::DisplayController::UC8279C:
      return "UC8279C";
    case BoardConfig::DisplayController::UC8179:
      return "UC8179";
    case BoardConfig::DisplayController::UC8279:
      return "UC8279";
    case BoardConfig::DisplayController::Epdiy:
      return "EPDiy";
    case BoardConfig::DisplayController::SSD1677:
    default:
      return "SSD1677";
  }
}


void HalDisplay::begin() {
  if (!kindle_screen::begin(kindle_geometry::get().width, kindle_geometry::get().height)) {
    // Without a screen the user could not even see how to leave, so quit and
    // let the launcher bring the Kindle UI back. Exit code 3 = no display.
    std::cerr << "[kindle] display unavailable; exiting" << std::endl;
    _exit(3);
  }
}

void HalDisplay::begin(bool /*seamless*/) { begin(); }

void HalDisplay::clearScreen(uint8_t color) const {
  memset(getFrameBuffer(), color, kindle_geometry::get().bufferSize);
  sim_screen_text::clear();
}

void HalDisplay::drawImage(const uint8_t *imageData, uint16_t x, uint16_t y,
                           uint16_t w, uint16_t h, bool) const {
  uint8_t *fb = getFrameBuffer();
  const uint16_t imageWidthBytes = w / 8;
  for (uint16_t row = 0; row < h; row++) {
    const uint16_t destY = y + row;
    if (destY >= kindle_geometry::get().height)
      break;
    // Kindle: size_t, not uint16_t (the buffer is over 64 KiB).
    const size_t destOffset = destY * kindle_geometry::get().widthBytes + (x / 8);
    const size_t srcOffset = row * imageWidthBytes;
    for (uint16_t col = 0; col < imageWidthBytes; col++) {
      if ((x / 8 + col) >= kindle_geometry::get().widthBytes)
        break;
      fb[destOffset + col] = imageData[srcOffset + col];
    }
  }
}

void HalDisplay::drawImageTransparent(const uint8_t *imageData, uint16_t x,
                                      uint16_t y, uint16_t w, uint16_t h,
                                      bool) const {
  uint8_t *fb = getFrameBuffer();
  const uint16_t imageWidthBytes = w / 8;
  for (uint16_t row = 0; row < h; row++) {
    const uint16_t destY = y + row;
    if (destY >= kindle_geometry::get().height)
      break;
    // Kindle: size_t, not uint16_t (the buffer is over 64 KiB).
    const size_t destOffset = destY * kindle_geometry::get().widthBytes + (x / 8);
    const size_t srcOffset = row * imageWidthBytes;
    for (uint16_t col = 0; col < imageWidthBytes; col++) {
      if ((x / 8 + col) >= kindle_geometry::get().widthBytes)
        break;
      fb[destOffset + col] &= imageData[srcOffset + col];
    }
  }
}

void HalDisplay::setInverted(bool value) { inverted = value; }

bool HalDisplay::toggleInverted() {
  inverted = !inverted;
  return inverted;
}

bool HalDisplay::isInverted() const { return inverted; }

void HalDisplay::displayBuffer(RefreshMode mode, bool turnOffScreen) {
  refreshDisplay(mode, turnOffScreen);
  if (std::this_thread::get_id() == simulatorMainThread) {
    presentIfNeeded();
  }
}

void HalDisplay::displayBufferAsync(RefreshMode mode) {
  // SDL presentation is already handed off to the main thread. The framebuffer
  // conversion itself remains synchronous, so advertise no genuine overlap.
  refreshDisplay(mode, false);
}

void HalDisplay::waitRefreshComplete() {}

bool HalDisplay::supportsAsyncRefresh() const { return false; }

void HalDisplay::displayWindow(int, int, int, int) {
  refreshDisplay(RefreshMode::FAST_REFRESH, false);
}

// Called from the render task (background thread): convert framebuffer to
// pixels and flag for present.
void HalDisplay::refreshDisplay(RefreshMode mode, bool /*turnOffScreen*/) {
  const uint8_t *fb = getFrameBuffer();
  if (!fb)
    return;
  snapshotBwBase(fb);
  renderBwPixels(fb);
  pendingPresent.store(false);
  basePending = false;
  presentToPanel(waveformFor(mode));
  sim_screen_text::flush();
}

// Kindle: frames are pushed from refreshDisplay()/displayGrayBuffer() on the
// calling thread; FBInk has no main-thread requirement.
void HalDisplay::presentIfNeeded() { pendingPresent.store(false); }

bool HalDisplay::shouldQuit() const { return quitRequested.load(); }

void HalDisplay::deepSleep() { presentIfNeeded(); }

uint8_t *HalDisplay::getFrameBuffer() const {
  if (frameBufferLent) {
    return nullptr;
  }
  return frameBufferStorage.data();
}

uint8_t *HalDisplay::lendFrameBufferStorage(uint32_t *sizeOut) {
  if (sizeOut) {
    *sizeOut = frameBufferLent ? 0 : kindle_geometry::get().bufferSize;
  }
  if (frameBufferLent) {
    return nullptr;
  }
  frameBufferLent = true;
  return frameBufferStorage.data();
}

void HalDisplay::returnFrameBufferStorage() {
  if (!frameBufferLent) {
    return;
  }
  frameBufferStorage.fill(0xFF);
  frameBufferLent = false;
}

HalDisplay::Controller HalDisplay::getController() const {
  return BoardConfig::ACTIVE.displayController;
}

HalDisplay::GrayscaleCapabilities HalDisplay::grayscaleCapabilities(
    GrayscaleMode mode) const {
  if (mode == GrayscaleMode::Absolute || mode == GrayscaleMode::Direct) {
    return {GrayscaleEncoding::AbsolutePlanes,
            mode == GrayscaleMode::Direct ? GrayscaleBase::Combined : GrayscaleBase::Separate,
            true, false, false};
  }
  return {GrayscaleEncoding::OverlayMasks,
          combinesGrayscaleBase() ? GrayscaleBase::Combined
                                  : GrayscaleBase::Separate,
          true, false, false};
}

void HalDisplay::copyGrayscaleBuffers(const uint8_t *lsbBuffer,
                                      const uint8_t *msbBuffer) {
  copyGrayscaleLsbBuffers(lsbBuffer);
  copyGrayscaleMsbBuffers(msbBuffer);
}
void HalDisplay::displayGrayscaleBase(RefreshMode fallback,
                                      bool turnOffScreen) {
  grayscalePreviewState.absolute = false;
  if (combinesGrayscaleBase()) {
    snapshotBwBase(getFrameBuffer());
    basePending = true;
    pendingBaseMode = fallback;
    return;
  }
  displayBuffer(fallback, turnOffScreen);
}
bool HalDisplay::displayGrayscaleBase(GrayscaleMode mode,
                                      RefreshMode fallback,
                                      bool turnOffScreen) {
  if (!grayscaleCapabilities(mode).supported()) return false;
  grayscalePreviewState.absolute = mode != GrayscaleMode::Overlay;
  if (combinesGrayscaleBase() && mode == GrayscaleMode::Overlay) {
    snapshotBwBase(getFrameBuffer());
    basePending = true;
    pendingBaseMode = fallback;
    return true;
  }
  displayBuffer(fallback, turnOffScreen);
  return true;
}
void HalDisplay::preconditionGrayscale() {}
void HalDisplay::preconditionGrayscale(uint16_t, uint16_t, uint16_t, uint16_t) {
}
void HalDisplay::copyGrayscaleLsbBuffers(const uint8_t *lsbBuffer) {
  copyPlane(grayscalePreviewState.lsbPlane, lsbBuffer,
            grayscalePreviewState.lsbValid);
}
void HalDisplay::copyGrayscaleMsbBuffers(const uint8_t *msbBuffer) {
  copyPlane(grayscalePreviewState.msbPlane, msbBuffer,
            grayscalePreviewState.msbValid);
}
void HalDisplay::cleanupGrayscaleBuffers(const uint8_t *bwBuffer) {
  if (bwBuffer && basePending) {
    // The gray pass never came (e.g. no memory for it): show the B/W page.
    basePending = false;
    snapshotBwBase(bwBuffer);
    renderBwPixels(bwBuffer);
    presentToPanel(waveformFor(pendingBaseMode));
    return;
  }
  if (bwBuffer) {
    snapshotBwBase(bwBuffer);
  } else {
    grayscalePreviewState.bwBaseValid = false;
    grayscalePreviewState.bwBase.fill(0);
    clearGrayscalePlanes();
  }
}
void HalDisplay::displayGrayBuffer(bool, const unsigned char *, bool) {
  composeGrayscalePreview();
  pendingPresent.store(false);
  const bool combined = basePending;
  basePending = false;
  // Combined page: use the refresh the base asked for (fast page turn, or the
  // periodic half refresh). A standalone gray overlay gets a clean GC16.
  presentToPanel(combined ? waveformFor(pendingBaseMode)
                          : kindle_screen::Waveform::Quality);
}

void HalDisplay::writeGrayscalePlaneStrip(bool lsbPlane, const uint8_t *rows,
                                          uint16_t yStart, uint16_t numRows) {
  if (!rows || numRows == 0 || yStart >= kindle_geometry::get().height) {
    return;
  }

  const uint16_t rowsToCopy =
      (yStart + numRows > kindle_geometry::get().height) ? (kindle_geometry::get().height - yStart) : numRows;
  const size_t offset = static_cast<size_t>(yStart) * kindle_geometry::get().widthBytes;
  const size_t byteCount =
      static_cast<size_t>(rowsToCopy) * kindle_geometry::get().widthBytes;
  auto &plane = lsbPlane ? grayscalePreviewState.lsbPlane
                         : grayscalePreviewState.msbPlane;
  memcpy(plane.data() + offset, rows, byteCount);
  if (lsbPlane) {
    grayscalePreviewState.lsbValid = true;
  } else {
    grayscalePreviewState.msbValid = true;
  }
}
bool HalDisplay::supportsStripGrayscale() const { return true; }
bool HalDisplay::supportsAsyncGrayscaleBase() const {
  return grayscaleCapabilities().asyncBase;
}
bool HalDisplay::combinesGrayscaleBase() const { return true; }

uint16_t HalDisplay::getDisplayWidth() const { return kindle_geometry::get().width; }
uint16_t HalDisplay::getDisplayHeight() const { return kindle_geometry::get().height; }
uint16_t HalDisplay::getDisplayWidthBytes() const {
  return kindle_geometry::get().widthBytes;
}
uint32_t HalDisplay::getBufferSize() const { return kindle_geometry::get().bufferSize; }

HalDisplay display;
