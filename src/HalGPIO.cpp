// Input for the Kindle build. Originally the simulator's SDL keyboard/mouse
// HAL; on the Kindle there is no SDL (KindleSdlCompat.h stubs it) and input
// comes from the touchscreen and power key (KindleInput.cpp) and the cover
// sensor. Kindle additions, all marked "Kindle:" or kindle*():
//   - touch: panel -> firmware coordinates, the menus' left-strip Back,
//     two-finger tap = Home (KindleInput.cpp);
//   - power key and cover = sleep; kernel suspend and wake
//     (kindleSleepUntilWake);
//   - the idle main-loop wait (kindleIdleWait) and the power log hooks;
//   - hooks the patched firmware calls: kindleSetActivity, kindleRequestExit.
// The simulator's scripted input (CROSSPOINT_SIM_INPUT_SCRIPT) is kept for
// tests run under qemu.

#include "HalGPIO.h"

#include <BoardConfig.h>
#include <GfxRenderer.h>
#include "KindleInput.h"
#include "KindleNet.h"
#include "KindlePowerLog.h"
#include "KindleScreen.h"
#include "KindleSdlCompat.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <regex>
#include <string>
#include <vector>
#include <ctime>

#include <glob.h>
#include <unistd.h>

#include "HalDisplay.h"
#include "KindleGeometry.h"
#include "SimLog.h"
#include "SimulatorLifecycle.h"

// Defined in HalDisplay.cpp — set here so all SDL event polling lives in one
// place.
extern std::atomic<bool> quitRequested;
extern GfxRenderer renderer;
// Defined in HalDisplay.cpp: saves the next presented frame to `path`.
void scheduleSimulatorScreenshot(const std::string &path);

void kindleTouchUp(float nx, float ny);  // below; shared by real and scripted taps
static bool readerActive = false;    // see kindleSetActivity()
static bool keyboardActive = false;

// Desktop simulator keyboard mapping (kept from the simulator; the Kindle
// has no keyboard, so these only matter for SDL stubs and scripted KEY input):
//   BTN_BACK    (0) → Escape
//   BTN_CONFIRM (1) → Return
//   BTN_LEFT    (2) → Left arrow
//   BTN_RIGHT   (3) → Right arrow
//   BTN_UP      (4) → Up arrow
//   BTN_DOWN    (5) → Down arrow
//   BTN_POWER   (6) → P
//   Simulator sleep shortcut → S

static constexpr int NUM_BUTTONS = 7;
static constexpr SDL_Scancode SIMULATOR_SLEEP_SCANCODE = SDL_SCANCODE_S;
static constexpr SDL_Scancode HOME_KEY_SCANCODE = SDL_SCANCODE_H;
static constexpr SDL_Scancode SLIDE_SWITCH_SCANCODE = SDL_SCANCODE_T;
// Kindle (300 ppi panel): a finger may drift up to ~4 mm and still count as
// a tap; a swipe must travel at least ~5 mm.
static constexpr int TOUCH_TAP_SLOP_PX = 48;
static constexpr int TOUCH_SWIPE_MIN_PX = 60;
// Kindle: 700 ms rejected slow, deliberate pulls (e.g. the frontlight
// pull-down); a finger on a 6" panel often takes longer.
static constexpr unsigned long TOUCH_SWIPE_MAX_MS = 1500;
static constexpr unsigned long TOUCH_LONG_PRESS_MS = 500;
static constexpr unsigned long HOME_KEY_LONG_PRESS_MS = 700;

static const SDL_Scancode buttonScancode[NUM_BUTTONS] = {
    SDL_SCANCODE_ESCAPE, // BTN_BACK
    SDL_SCANCODE_RETURN, // BTN_CONFIRM
    SDL_SCANCODE_LEFT,   // BTN_LEFT
    SDL_SCANCODE_RIGHT,  // BTN_RIGHT
    SDL_SCANCODE_UP,     // BTN_UP
    SDL_SCANCODE_DOWN,   // BTN_DOWN
    SDL_SCANCODE_P,      // BTN_POWER
};

static bool pressedThisFrame[NUM_BUTTONS] = {};
static bool releasedThisFrame[NUM_BUTTONS] = {};
static unsigned long buttonPressTime[NUM_BUTTONS] = {};
static bool syntheticButtonDown[NUM_BUTTONS] = {};
static bool simulatorSleepRequested = false;
static bool coverButtonDown[NUM_BUTTONS] = {};
static bool capacitivePagePressedThisFrame = false;

namespace {

struct TouchState {
  bool down = false;
  bool pressedThisFrame = false;
  bool releasedThisFrame = false;
  bool movedBeyondTapSlop = false;
  bool longPressThisFrame = false;
  bool longPressFired = false;
  bool suppressed = false;
  bool activityThisFrame = false;
  float startNx = 0.0f;
  float startNy = 0.0f;
  float currentNx = 0.0f;
  float currentNy = 0.0f;
  unsigned long pressedAt = 0;
  unsigned long lastHeldMs = 0;
};

TouchState touchState;
bool homeKeyDown = false;
bool homeKeyPressedThisFrame = false;
bool homeKeyTappedThisFrame = false;
bool homeKeyLongPressedThisFrame = false;
bool homeKeyLongFired = false;
unsigned long homeKeyPressedAt = 0;
bool toggleSwitchOn = false;

void setToggleSwitch(bool on) {
  if (BoardConfig::ACTIVE.input.toggleSwitch < 0)
    return;
  toggleSwitchOn = on;
}

enum class SyntheticAction {
  KeyDown,
  KeyUp,
  CoverDown,
  CoverUp,
  TouchDown,
  TouchUp,
  HomeDown,
  HomeUp,
  SwitchOn,
  SwitchOff,
  Sleep,
  Quit,
  Wait,
  Screenshot
};

struct SyntheticEvent {
  unsigned long atMs;
  SyntheticAction action;
  int button = -1;
  float logicalNx = 0.0f;
  float logicalNy = 0.0f;
  bool handled = false;
  // Each WAIT starts a new segment; atMs counts from the segment start.
  size_t segment = 0;
  std::string text{}; // WAIT pattern or SHOT path
  std::regex pattern{};
  unsigned long waitingSinceMs = 0;
};

std::vector<SyntheticEvent> syntheticEvents;
bool syntheticEventsInitialized = false;
// millis() when each script segment started; segment 0 starts at boot.
std::vector<long> segmentStartMs;
// Start of the last scripted input, so WAIT also sees lines it caused.
unsigned long lastInputMs = 0;
unsigned long waitTimeoutMs = 30000;

float clamp01(float value) { return std::max(0.0f, std::min(1.0f, value)); }

bool isPortraitOrientation() {
  const auto o = renderer.getOrientation();
  return o == GfxRenderer::Portrait || o == GfxRenderer::PortraitInverted;
}

int orientedPanelWidth() {
  return isPortraitOrientation() ? kindle_geometry::get().height
                                 : kindle_geometry::get().width;
}

int orientedPanelHeight() {
  return isPortraitOrientation() ? kindle_geometry::get().width
                                 : kindle_geometry::get().height;
}

void logicalToPanelNormalized(float logicalNx, float logicalNy, float &panelNx,
                              float &panelNy) {
  const int logicalWidth = orientedPanelWidth();
  const int logicalHeight = orientedPanelHeight();
  const int lx = static_cast<int>(clamp01(logicalNx) *
                                  static_cast<float>(logicalWidth - 1));
  const int ly = static_cast<int>(clamp01(logicalNy) *
                                  static_cast<float>(logicalHeight - 1));

  int physicalX = 0;
  int physicalY = 0;
  switch (renderer.getOrientation()) {
  case GfxRenderer::Portrait:
    physicalX = ly;
    physicalY = kindle_geometry::get().height - 1 - lx;
    break;
  case GfxRenderer::PortraitInverted:
    physicalX = kindle_geometry::get().width - 1 - ly;
    physicalY = lx;
    break;
  case GfxRenderer::LandscapeClockwise:
    physicalX = kindle_geometry::get().width - 1 - lx;
    physicalY = kindle_geometry::get().height - 1 - ly;
    break;
  case GfxRenderer::LandscapeCounterClockwise:
  default:
    physicalX = lx;
    physicalY = ly;
    break;
  }

  panelNx = clamp01(static_cast<float>(physicalX) /
                    static_cast<float>(kindle_geometry::get().width - 1));
  panelNy = clamp01(static_cast<float>(physicalY) /
                    static_cast<float>(kindle_geometry::get().height - 1));
}

void updateTouchMovement(float panelNx, float panelNy) {
  touchState.currentNx = panelNx;
  touchState.currentNy = panelNy;
  const float dx =
      (touchState.currentNx - touchState.startNx) * kindle_geometry::get().width;
  const float dy =
      (touchState.currentNy - touchState.startNy) * kindle_geometry::get().height;
  if (std::abs(dx) > TOUCH_TAP_SLOP_PX || std::abs(dy) > TOUCH_TAP_SLOP_PX) {
    touchState.movedBeyondTapSlop = true;
  }
}

void beginTouch(float logicalNx, float logicalNy) {
  if (!BoardConfig::hasTouch())
    return;
  float panelNx = 0.0f;
  float panelNy = 0.0f;
  logicalToPanelNormalized(logicalNx, logicalNy, panelNx, panelNy);
  touchState.down = true;
  touchState.pressedThisFrame = true;
  touchState.activityThisFrame = true;
  touchState.movedBeyondTapSlop = false;
  touchState.longPressThisFrame = false;
  touchState.longPressFired = false;
  touchState.suppressed = false;
  touchState.startNx = panelNx;
  touchState.startNy = panelNy;
  touchState.currentNx = panelNx;
  touchState.currentNy = panelNy;
  touchState.pressedAt = SDL_GetTicks();
}

void moveTouch(float logicalNx, float logicalNy) {
  if (!touchState.down)
    return;
  float panelNx = 0.0f;
  float panelNy = 0.0f;
  logicalToPanelNormalized(logicalNx, logicalNy, panelNx, panelNy);
  updateTouchMovement(panelNx, panelNy);
}

void endTouch(float logicalNx, float logicalNy) {
  if (!touchState.down)
    return;
  moveTouch(logicalNx, logicalNy);
  touchState.down = false;
  touchState.releasedThisFrame = true;
  touchState.activityThisFrame = true;
  touchState.lastHeldMs = SDL_GetTicks() - touchState.pressedAt;
  static const bool debugTouch = std::getenv("CPK_DEBUG_TOUCH") != nullptr;
  if (debugTouch)
    std::fprintf(stderr,
                 "[kindle] touch up: start=(%.3f,%.3f) end=(%.3f,%.3f) held=%lums "
                 "moved=%d suppressed=%d logical=(%.3f,%.3f) reader=%d keyboard=%d\n",
                 touchState.startNx, touchState.startNy, touchState.currentNx,
                 touchState.currentNy, touchState.lastHeldMs,
                 touchState.movedBeyondTapSlop ? 1 : 0,
                 touchState.suppressed ? 1 : 0, logicalNx, logicalNy,
                 readerActive ? 1 : 0, keyboardActive ? 1 : 0);
}

void updateTouchHold() {
  if (touchState.down && !touchState.movedBeyondTapSlop &&
      !touchState.longPressFired && !touchState.suppressed &&
      SDL_GetTicks() - touchState.pressedAt >= TOUCH_LONG_PRESS_MS) {
    touchState.longPressFired = true;
    touchState.longPressThisFrame = true;
  }
}

void beginHomeKey() {
  if (!BoardConfig::hasHomeKey() || homeKeyDown)
    return;
  homeKeyDown = true;
  homeKeyPressedThisFrame = true;
  homeKeyLongFired = false;
  homeKeyPressedAt = SDL_GetTicks();
}

void endHomeKey() {
  if (!homeKeyDown)
    return;
  if (!homeKeyLongFired &&
      SDL_GetTicks() - homeKeyPressedAt < HOME_KEY_LONG_PRESS_MS) {
    homeKeyTappedThisFrame = true;
  }
  homeKeyDown = false;
}

void updateHomeKeyHold() {
  if (homeKeyDown && !homeKeyLongFired &&
      SDL_GetTicks() - homeKeyPressedAt >= HOME_KEY_LONG_PRESS_MS) {
    homeKeyLongFired = true;
    homeKeyLongPressedThisFrame = true;
  }
}

bool parseTouchSpec(const std::string &detail, float &x1, float &y1, float &x2,
                    float &y2, unsigned long &duration, bool swipe) {
  unsigned parsedDuration = swipe ? 250 : 80;
  int parsed = 0;
  if (swipe) {
    parsed = std::sscanf(detail.c_str(), "%f,%f,%f,%f,%u", &x1, &y1, &x2, &y2,
                         &parsedDuration);
    if (parsed < 4)
      return false;
  } else {
    parsed = std::sscanf(detail.c_str(), "%f,%f,%u", &x1, &y1, &parsedDuration);
    if (parsed < 2)
      return false;
    x2 = x1;
    y2 = y1;
  }

  // Scripts normally use logical display pixels because those coordinates are
  // easy to read from UI layouts and screenshots. Preserve support for the
  // earlier 0.0-1.0 normalized form so existing local QA scripts keep working.
  const auto normalize = [](float value, int extent) {
    if (value >= 0.0f && value <= 1.0f)
      return value;
    return clamp01(value / static_cast<float>(std::max(1, extent - 1)));
  };
  const int logicalWidth = orientedPanelWidth();
  const int logicalHeight = orientedPanelHeight();
  x1 = normalize(x1, logicalWidth);
  y1 = normalize(y1, logicalHeight);
  x2 = normalize(x2, logicalWidth);
  y2 = normalize(y2, logicalHeight);
  duration = parsedDuration;
  return true;
}

void requestSimulatorSleep() {
  simulatorSleepRequested = true;
  // Current CrossPoint firmware sleeps on a held physical power button. Keep
  // the compatibility latch above for older consumers, and also drive the
  // current public HalGPIO state so the S shortcut follows the real firmware
  // sleep path.
  pressedThisFrame[HalGPIO::BTN_POWER] = true;
  syntheticButtonDown[HalGPIO::BTN_POWER] = true;
  buttonPressTime[HalGPIO::BTN_POWER] = SDL_GetTicks();
}

bool buttonAvailable(int button) {
  if (BoardConfig::isPicco())
    return button == HalGPIO::BTN_CONFIRM || button == HalGPIO::BTN_POWER ||
           button == HalGPIO::BTN_UP || button == HalGPIO::BTN_DOWN;
  if (BoardConfig::isEegoA4())
    return button == HalGPIO::BTN_POWER || button == HalGPIO::BTN_UP ||
           button == HalGPIO::BTN_DOWN;
  if (!BoardConfig::isMetalioEInk4() && !BoardConfig::isMetalioEInk4Plus())
    return true;
  // BOOT/POWER and the expander/cover page keys are the only button inputs.
  // Kindle: Back too, synthesized by the left-edge tap strip in menus.
  return button == HalGPIO::BTN_CONFIRM || button == HalGPIO::BTN_POWER ||
         button == HalGPIO::BTN_BACK ||
         button == HalGPIO::BTN_UP || button == HalGPIO::BTN_DOWN;
}

void beginCoverButton(int button) {
  if ((!BoardConfig::isMetalioEInk4() && !BoardConfig::isMetalioEInk4Plus()) ||
      coverButtonDown[button])
    return;
  coverButtonDown[button] = true;
  capacitivePagePressedThisFrame = true;
  if (!syntheticButtonDown[button] &&
      !SDL_GetKeyboardState(nullptr)[buttonScancode[button]]) {
    pressedThisFrame[button] = true;
    buttonPressTime[button] = SDL_GetTicks();
  }
}

void endCoverButton(int button) {
  if (!coverButtonDown[button])
    return;
  coverButtonDown[button] = false;
  if (!syntheticButtonDown[button] &&
      !SDL_GetKeyboardState(nullptr)[buttonScancode[button]])
    releasedThisFrame[button] = true;
}

std::string uppercase(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return value;
}

int namedButton(const std::string &name) {
  if (name == "ESCAPE" || name == "BACK")
    return HalGPIO::BTN_BACK;
  if (name == "RETURN" || name == "ENTER" || name == "CONFIRM")
    return HalGPIO::BTN_CONFIRM;
  if (name == "LEFT")
    return HalGPIO::BTN_LEFT;
  if (name == "RIGHT")
    return HalGPIO::BTN_RIGHT;
  if (name == "UP")
    return HalGPIO::BTN_UP;
  if (name == "DOWN")
    return HalGPIO::BTN_DOWN;
  if (name == "P" || name == "POWER")
    return HalGPIO::BTN_POWER;
  return -1;
}

void initializeSyntheticEvents() {
  if (syntheticEventsInitialized)
    return;
  syntheticEventsInitialized = true;

  const char *script = std::getenv("CROSSPOINT_SIM_INPUT_SCRIPT");
  if (!script || script[0] == '\0')
    return;

  if (const char *timeout = std::getenv("CROSSPOINT_SIM_WAIT_TIMEOUT_MS"))
    waitTimeoutMs = std::strtoul(timeout, nullptr, 10);

  const std::string spec(script);
  size_t segment = 0;
  size_t start = 0;
  while (start < spec.size()) {
    const size_t end = spec.find(';', start);
    const std::string item = spec.substr(
        start, end == std::string::npos ? std::string::npos : end - start);
    const size_t firstEvent = syntheticEvents.size();
    const size_t firstColon = item.find(':');
    const size_t secondColon = firstColon == std::string::npos
                                   ? std::string::npos
                                   : item.find(':', firstColon + 1);
    if (firstColon != std::string::npos) {
      const unsigned long atMs =
          std::strtoul(item.substr(0, firstColon).c_str(), nullptr, 10);
      const std::string key = uppercase(
          item.substr(firstColon + 1, secondColon == std::string::npos
                                          ? std::string::npos
                                          : secondColon - firstColon - 1));
      if (key == "SWITCH_ON" || key == "SWITCH_OFF") {
        syntheticEvents.push_back({atMs, key == "SWITCH_ON"
                                             ? SyntheticAction::SwitchOn
                                             : SyntheticAction::SwitchOff});
      } else if (key == "QUIT") {
        syntheticEvents.push_back({atMs, SyntheticAction::Quit});
      } else if ((key == "WAIT" || key == "SHOT") &&
                 secondColon != std::string::npos) {
        SyntheticEvent event{atMs, key == "WAIT" ? SyntheticAction::Wait
                                                 : SyntheticAction::Screenshot};
        event.text = item.substr(secondColon + 1);
        if (key == "WAIT") {
          try {
            event.pattern = std::regex(event.text);
          } catch (const std::regex_error &e) {
            std::fprintf(stderr, "[SIM] Bad WAIT pattern \"%s\": %s\n",
                         event.text.c_str(), e.what());
            SimulatorLifecycle::setExitCode(2);
            quitRequested.store(true);
          }
        }
        syntheticEvents.push_back(std::move(event));
      } else if (key == "S" || key == "SLEEP") {
        syntheticEvents.push_back({atMs, SyntheticAction::Sleep});
      } else if (key == "PREV" || key == "NEXT") {
        const int button = key == "PREV" ? HalGPIO::BTN_UP : HalGPIO::BTN_DOWN;
        const unsigned long holdMs =
            secondColon == std::string::npos
                ? 80
                : std::strtoul(item.substr(secondColon + 1).c_str(), nullptr,
                               10);
        syntheticEvents.push_back({atMs, SyntheticAction::CoverDown, button});
        syntheticEvents.push_back(
            {atMs + holdMs, SyntheticAction::CoverUp, button});
      } else if (key == "HOME") {
        const unsigned long holdMs =
            secondColon == std::string::npos
                ? 80
                : std::strtoul(item.substr(secondColon + 1).c_str(), nullptr,
                               10);
        syntheticEvents.push_back({atMs, SyntheticAction::HomeDown});
        syntheticEvents.push_back({atMs + holdMs, SyntheticAction::HomeUp});
      } else if ((key == "TAP" || key == "SWIPE") &&
                 secondColon != std::string::npos) {
        float x1 = 0.0f;
        float y1 = 0.0f;
        float x2 = 0.0f;
        float y2 = 0.0f;
        unsigned long duration = 0;
        const bool swipe = key == "SWIPE";
        if (parseTouchSpec(item.substr(secondColon + 1), x1, y1, x2, y2,
                           duration, swipe)) {
          syntheticEvents.push_back(
              {atMs, SyntheticAction::TouchDown, -1, x1, y1});
          syntheticEvents.push_back(
              {atMs + duration, SyntheticAction::TouchUp, -1, x2, y2});
        }
      } else {
        const int button = namedButton(key);
        if (button >= 0) {
          const unsigned long holdMs =
              secondColon == std::string::npos
                  ? 80
                  : std::strtoul(item.substr(secondColon + 1).c_str(), nullptr,
                                 10);
          syntheticEvents.push_back({atMs, SyntheticAction::KeyDown, button});
          syntheticEvents.push_back(
              {atMs + holdMs, SyntheticAction::KeyUp, button});
        }
      }
    }

    for (size_t i = firstEvent; i < syntheticEvents.size(); ++i) {
      syntheticEvents[i].segment = segment;
      if (syntheticEvents[i].action == SyntheticAction::Wait)
        ++segment;
    }

    if (end == std::string::npos)
      break;
    start = end + 1;
  }

  segmentStartMs.assign(segment + 1, -1);
  segmentStartMs[0] = 0;
  std::stable_sort(syntheticEvents.begin(), syntheticEvents.end(),
                   [](const SyntheticEvent &a, const SyntheticEvent &b) {
                     return a.segment != b.segment ? a.segment < b.segment
                                                   : a.atMs < b.atMs;
                   });
}

// True once a WAIT's pattern is seen; times out the run after waitTimeoutMs.
bool waitFinished(SyntheticEvent &event, unsigned long now) {
  std::string line;
  if (sim_log::findSince(lastInputMs, event.pattern, &line)) {
    std::fprintf(stderr, "[SIM] WAIT matched: %s\n", line.c_str());
    segmentStartMs[event.segment + 1] = static_cast<long>(now);
    return true;
  }
  if (event.waitingSinceMs == 0) {
    event.waitingSinceMs = now;
  } else if (now - event.waitingSinceMs > waitTimeoutMs) {
    std::fprintf(stderr, "[SIM] WAIT timed out after %lu ms: %s\n",
                 waitTimeoutMs, event.text.c_str());
    SimulatorLifecycle::setExitCode(1);
    quitRequested.store(true);
    return true;
  }
  return false;
}

void processSyntheticEvents() {
  initializeSyntheticEvents();
  const unsigned long now = millis();
  for (auto &event : syntheticEvents) {
    const long segmentStart = segmentStartMs[event.segment];
    if (event.handled || segmentStart < 0 ||
        static_cast<unsigned long>(segmentStart) + event.atMs > now)
      continue;
    if (event.action == SyntheticAction::Wait) {
      event.handled = waitFinished(event, now);
      continue;
    }
    event.handled = true;
    switch (event.action) {
    case SyntheticAction::KeyDown:
    case SyntheticAction::CoverDown:
    case SyntheticAction::TouchDown:
    case SyntheticAction::HomeDown:
    case SyntheticAction::SwitchOn:
    case SyntheticAction::SwitchOff:
    case SyntheticAction::Sleep:
      lastInputMs = now;
      break;
    default:
      break;
    }
    switch (event.action) {
    case SyntheticAction::KeyDown:
      if (!buttonAvailable(event.button))
        break;
      // Held-time calculations use SDL_GetTicks() for real keyboard events;
      // synthetic presses must use the same clock origin to avoid unsigned
      // underflow being mistaken for an immediate long press.
      if (!coverButtonDown[event.button]) {
        pressedThisFrame[event.button] = true;
        buttonPressTime[event.button] = SDL_GetTicks();
      }
      syntheticButtonDown[event.button] = true;
      break;
    case SyntheticAction::KeyUp:
      if (!buttonAvailable(event.button))
        break;
      if (!coverButtonDown[event.button])
        releasedThisFrame[event.button] = true;
      syntheticButtonDown[event.button] = false;
      break;
    case SyntheticAction::CoverDown:
      beginCoverButton(event.button);
      break;
    case SyntheticAction::CoverUp:
      endCoverButton(event.button);
      break;
    case SyntheticAction::TouchDown:
      beginTouch(event.logicalNx, event.logicalNy);
      break;
    case SyntheticAction::TouchUp:
      kindleTouchUp(event.logicalNx, event.logicalNy);
      break;
    case SyntheticAction::HomeDown:
      beginHomeKey();
      break;
    case SyntheticAction::HomeUp:
      endHomeKey();
      break;
    case SyntheticAction::SwitchOn:
      setToggleSwitch(true);
      break;
    case SyntheticAction::SwitchOff:
      setToggleSwitch(false);
      break;
    case SyntheticAction::Sleep:
      requestSimulatorSleep();
      break;
    case SyntheticAction::Quit:
      quitRequested.store(true);
      break;
    case SyntheticAction::Screenshot:
      scheduleSimulatorScreenshot(event.text);
      break;
    case SyntheticAction::Wait:
      break;
    }
  }
}

} // namespace

static void clearButtonState() {
  for (int i = 0; i < NUM_BUTTONS; i++) {
    pressedThisFrame[i] = false;
    releasedThisFrame[i] = false;
    buttonPressTime[i] = 0;
    syntheticButtonDown[i] = false;
    coverButtonDown[i] = false;
  }
  touchState = {};
  homeKeyDown = false;
  homeKeyPressedThisFrame = false;
  homeKeyTappedThisFrame = false;
  homeKeyLongPressedThisFrame = false;
  homeKeyLongFired = false;
  homeKeyPressedAt = 0;
}

static int scancodeToButton(SDL_Scancode sc) {
  for (int i = 0; i < NUM_BUTTONS; i++) {
    if (buttonScancode[i] == sc)
      return i;
  }
  return -1;
}

// Kindle: convert a touch in portrait panel coordinates (u right, v down, the
// Paperwhite held upright) into the simulator's "logical window" coordinates,
// which logicalToPanelNormalized() maps back into the firmware buffer.
// The panel shows the buffer rotated 90 degrees CW (HalDisplay panelMapping),
// so the buffer point is bx = v, by = 1 - u; each case below inverts
// logicalToPanelNormalized() for that orientation.
static void panelToLogical(float u, float v, float &logicalNx,
                           float &logicalNy) {
  const float bx = v;
  const float by = 1.0f - u;
  switch (renderer.getOrientation()) {
  case GfxRenderer::Portrait:
    logicalNx = 1.0f - by;
    logicalNy = bx;
    break;
  case GfxRenderer::PortraitInverted:
    logicalNx = by;
    logicalNy = 1.0f - bx;
    break;
  case GfxRenderer::LandscapeClockwise:
    logicalNx = 1.0f - bx;
    logicalNy = 1.0f - by;
    break;
  case GfxRenderer::LandscapeCounterClockwise:
  default:
    logicalNx = bx;
    logicalNy = by;
    break;
  }
}

// Which screen is showing (set every frame by the patched ActivityManager).
void kindleSetActivity(const char *name, bool reader) {
  readerActive = reader;
  keyboardActive = name && std::strstr(name, "Keyboard") != nullptr;
}

// Menus: a tap in the left 10% (below the top strip) is Back. Press now,
// release on the next frame, like a real button.
static constexpr float kBackStripFrac = 0.10f;
static constexpr float kTopStripFrac = 1.0f / 12.0f;
static unsigned long frameNo = 0;
static bool coverPressPending = false;  // see pollCover()
static unsigned long coverPressFrame = 0;
static void pollCover();
static unsigned long backPressedFrame = 0;
static bool backPending = false;

static bool isBackStripTap(float logicalNx, float logicalNy) {
  return !readerActive && !keyboardActive && logicalNx < kBackStripFrac &&
         logicalNy >= kTopStripFrac && !touchState.movedBeyondTapSlop &&
         !touchState.suppressed;
}

// Finger lifted: a normal tap/release, or a Back press in the menus' left strip.
// Real touches and scripted test taps both come through here.
void kindleTouchUp(float nx, float ny) {
  if (!touchState.down)
    return;
  moveTouch(nx, ny);
  if (isBackStripTap(nx, ny)) {
    static const bool debugTouch = std::getenv("CPK_DEBUG_TOUCH") != nullptr;
    if (debugTouch)
      std::fprintf(stderr, "[kindle] Back (left strip tap at %.3f,%.3f)\n", nx, ny);
    touchState.suppressed = true;  // a Back, not a tap on the row under it
    endTouch(nx, ny);
    pressedThisFrame[HalGPIO::BTN_BACK] = true;
    syntheticButtonDown[HalGPIO::BTN_BACK] = true;
    buttonPressTime[HalGPIO::BTN_BACK] = SDL_GetTicks();
    backPending = true;
    backPressedFrame = frameNo;
    return;
  }
  endTouch(nx, ny);
}

// Called by the patched Home menu's "Exit CrossPoint" entry.
void kindleRequestExit() {
  std::fprintf(stderr, "[kindle] exit requested from the menu\n");
  quitRequested.store(true);
}

// millis() at startup when this process is the restart after a wake; 0 otherwise.
static unsigned long kindleWokeAt = 0;
// Power key: the frame its press was reported, and a release held back to
// the next frame (see PowerUp below).
static unsigned long powerPressFrame = 0;
static bool powerReleasePending = false;

static void pollKindleTouch() {
  if (backPending && frameNo > backPressedFrame) {
    backPending = false;
    syntheticButtonDown[HalGPIO::BTN_BACK] = false;
    releasedThisFrame[HalGPIO::BTN_BACK] = true;
  }
  static std::vector<kindle_input::Event> events;
  events.clear();
  kindle_input::poll(events);
  for (const auto &ev : events) {
    float nx = 0.0f, ny = 0.0f;
    panelToLogical(ev.u, ev.v, nx, ny);
    switch (ev.type) {
    case kindle_input::Event::Type::Down:
      beginTouch(nx, ny);
      break;
    case kindle_input::Event::Type::Move:
      moveTouch(nx, ny);
      break;
    case kindle_input::Event::Type::Up:
      kindleTouchUp(nx, ny);
      break;
    case kindle_input::Event::Type::Cancel:
      if (touchState.down) {
        touchState.suppressed = true;
        endTouch(touchState.currentNx, touchState.currentNy);
      }
      break;
    case kindle_input::Event::Type::HomeTap:
      beginHomeKey();
      endHomeKey();
      break;
    case kindle_input::Event::Type::PowerDown:
      // Just woke up: extra presses from waking must not send it back to sleep.
      if (kindleWokeAt && millis() - kindleWokeAt < 3000) {
        std::fprintf(stderr, "[kindle] power press ignored (just woke)\n");
        break;
      }
      // Any press of the Kindle's power button means "sleep". The firmware
      // sleeps on a hold of more than 400 ms (a short press does nothing by
      // default), so report the press as already held that long.
      pressedThisFrame[HalGPIO::BTN_POWER] = true;
      syntheticButtonDown[HalGPIO::BTN_POWER] = true;
      buttonPressTime[HalGPIO::BTN_POWER] = std::max<uint32_t>(1, SDL_GetTicks() - 600);
      powerPressFrame = frameNo;
      powerReleasePending = false;
      break;
    case kindle_input::Event::Type::PowerUp:
      if (!syntheticButtonDown[HalGPIO::BTN_POWER])
        break;
      if (powerPressFrame == frameNo) {
        // A quick tap: press and release arrived in the same poll. Releasing
        // now would hide the hold from the firmware (held time 0 = no sleep),
        // so keep the button down until the next frame.
        powerReleasePending = true;
        break;
      }
      releasedThisFrame[HalGPIO::BTN_POWER] = true;
      syntheticButtonDown[HalGPIO::BTN_POWER] = false;
      break;
    }
  }
}

// Main loop, idle (patched into main.cpp): wait for input instead of waking
// every 10 ms. Anything in flight (a finger down, a button held, a release
// due next frame) gets the stock short delay instead. CPK_IDLE_WAIT_MS sets
// the longest wait (default 500 ms; 0 = stock behaviour). Scripted test runs
// keep the stock timing, since their input does not come from the devices.
void kindleIdleWait() {
  static const int waitMs = [] {
    const char *v = std::getenv("CPK_IDLE_WAIT_MS");
    int ms = v && *v ? std::atoi(v) : 500;
    if (std::getenv("CROSSPOINT_SIM_INPUT_SCRIPT") && !std::getenv("CPK_TEST_IDLE_WAIT"))
      ms = 0;
    std::fprintf(stderr, "[kindle] idle wait: %d ms\n", ms);
    return ms;
  }();
  const bool busy = gpio.rawInputActive() || powerReleasePending || coverPressPending ||
                    backPending;
  if (waitMs <= 0 || busy) {
    // Stock behaviour: 50 ms in 10 ms slices, ending early on input.
    const unsigned long idleStart = millis();
    while (millis() - idleStart < 50) {
      delay(10);
      if (gpio.rawInputActive())
        break;
    }
    return;
  }
  kindle_input::waitForInput(waitMs);
}

void kindleRadioOffForRestart() { kindle_net::setRadio(false); }

void HalGPIO::begin() {
  kindle_net::setRadio(false);  // no-op unless the launcher turned Wi-Fi off
#if defined(SIMULATOR_DEVICE_PICCO)
  _deviceType = DeviceType::X4;
  BoardConfig::selectDevice(BoardConfig::Board::Picco);
#elif defined(SIMULATOR_DEVICE_EEGO_A4)
  _deviceType = DeviceType::X4;
  BoardConfig::selectDevice(BoardConfig::Board::EegoA4);
#elif defined(SIMULATOR_DEVICE_METALIO_EINK4)
  _deviceType = DeviceType::X4;
  BoardConfig::selectDevice(BoardConfig::Board::MetalioEInk4);
#elif defined(SIMULATOR_DEVICE_METALIO_EINK4_PLUS)
  _deviceType = DeviceType::X4;
  BoardConfig::selectDevice(BoardConfig::Board::MetalioEInk4Plus);
#elif defined(SIMULATOR_DEVICE_PAPERMONO)
  _deviceType = DeviceType::X4;
  BoardConfig::selectDevice(BoardConfig::Board::PaperMono);
#elif defined(SIMULATOR_DEVICE_STICKY)
  // The firmware's non-C3 path leaves the legacy device discriminator on X4;
  // BoardConfig carries the actual Sticky identity and capabilities.
  _deviceType = DeviceType::X4;
  BoardConfig::selectDevice(BoardConfig::Board::Sticky);
#elif defined(SIMULATOR_DEVICE_X4_PRO)
  _deviceType = DeviceType::X4;
  BoardConfig::selectDevice(BoardConfig::Board::XteinkX4Pro);
#elif defined(SIMULATOR_DEVICE_X4_CLASSIC)
  _deviceType = DeviceType::X4;
  BoardConfig::selectDevice(BoardConfig::Board::XteinkX4Classic);
#elif defined(SIMULATOR_DEVICE_X3)
  _deviceType = DeviceType::X3;
#if defined(SIMULATOR_DISPLAY_UC8279)
  BoardConfig::selectDevice(BoardConfig::Board::XteinkX3Uc8279);
#else
  BoardConfig::selectDevice(BoardConfig::Board::XteinkX3);
#endif
#else
  _deviceType = DeviceType::X4;
  BoardConfig::selectDevice(BoardConfig::Board::XteinkX4);
#endif
  const char *position = std::getenv("CROSSPOINT_SIM_SLIDE_SWITCH");
  toggleSwitchOn = false;
  setToggleSwitch(position && std::strcmp(position, "on") == 0);
  // The firmware lays out for the zoomed buffer, not the physical panel.
  BoardConfig::ACTIVE.displayWidth = kindle_geometry::get().width;
  BoardConfig::ACTIVE.displayHeight = kindle_geometry::get().height;
  if (std::getenv("CPK_JUST_WOKE")) {
    unsetenv("CPK_JUST_WOKE");
    kindleWokeAt = std::max<unsigned long>(1, millis());
  }
  kindle_input::Orientation quirks;
  kindle_screen::touchQuirks(quirks.swapAxes, quirks.mirrorX, quirks.mirrorY);
  if (!kindle_input::begin(quirks) && !std::getenv("CPK_FAKE_FB")) {
    // No touchscreen means no way to operate or leave the app. Exit code 4.
    std::fprintf(stderr, "[kindle] touchscreen unavailable; exiting\n");
    _exit(4);
  }
  // A key a script presses at 0 ms is held at power-on, so setup() sees it
  // the way the device sees a held button (Back skips reopening the book).
  processSyntheticEvents();
}

bool HalGPIO::isXteinkDevice() const {
  // Match the firmware helper's narrower meaning: the runtime-detected C3
  // X3/X4 pair. X4 Pro and X4 Classic use their own S3 board profiles.
  return BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX3 ||
         BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX3Uc8279 ||
         BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX4;
}

bool HalGPIO::hasEdgeSideButtons() const {
  return BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX3 ||
         BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX3Uc8279 ||
         BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX4Pro ||
         BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX4Classic;
}

void HalGPIO::beginFrame() {
  ++frameNo;
  kindlePowerLogFrame();
  // Clear the press/release edge latches once per frame. See update() for why
  // this is deliberately separate from the SDL poll.
  for (int i = 0; i < NUM_BUTTONS; i++) {
    pressedThisFrame[i] = false;
    releasedThisFrame[i] = false;
  }
  capacitivePagePressedThisFrame = false;
  touchState.pressedThisFrame = false;
  touchState.releasedThisFrame = false;
  touchState.activityThisFrame = false;
  touchState.longPressThisFrame = false;
  homeKeyPressedThisFrame = false;
  homeKeyTappedThisFrame = false;
  homeKeyLongPressedThisFrame = false;
}

void HalGPIO::update() {
  // Per-frame press/release edges are intentionally NOT cleared here; that
  // happens once per frame in beginFrame(). The firmware calls update() several
  // times within a single frame (e.g. CrossPointWebServerActivity polls input
  // between handleClient() bursts, on top of the top-of-loop gpio.update() in
  // main.cpp). If edges were cleared on every update(), a key press drained by
  // an earlier update() would be wiped before a later update()'s wasPressed()
  // check could observe it — which made Back/Exit require repeated presses.
  // Latching edges for the whole frame keeps wasPressed() stable across all
  // update() calls in that frame, matching the on-device InputManager.

  // Kindle: touchscreen and power key events, then the deferred releases
  // (a release that arrived in the same poll as its press is reported one
  // frame later, so the firmware sees the press as held).
  pollKindleTouch();
  if (powerReleasePending && frameNo > powerPressFrame) {
    powerReleasePending = false;
    syntheticButtonDown[HalGPIO::BTN_POWER] = false;
    releasedThisFrame[HalGPIO::BTN_POWER] = true;
  }
  if (coverPressPending && frameNo > coverPressFrame) {
    coverPressPending = false;
    syntheticButtonDown[HalGPIO::BTN_POWER] = false;
    releasedThisFrame[HalGPIO::BTN_POWER] = true;
  }
  pollCover();
  kindlePowerLogTick();
  processSyntheticEvents();
  updateTouchHold();
  updateHomeKeyHold();
}

bool HalGPIO::isPressed(uint8_t buttonIndex) const {
  if (buttonIndex >= NUM_BUTTONS || !buttonAvailable(buttonIndex))
    return false;
  const uint8_t *state = SDL_GetKeyboardState(NULL);
  return state[buttonScancode[buttonIndex]] ||
         syntheticButtonDown[buttonIndex] || coverButtonDown[buttonIndex];
}

bool HalGPIO::rawInputActive() {
  for (uint8_t button = 0; button < NUM_BUTTONS; ++button) {
    if (isPressed(button)) return true;
  }
  return touchState.down || homeKeyDown;
}

bool HalGPIO::wasPressed(uint8_t buttonIndex) const {
  if (buttonIndex >= NUM_BUTTONS || !buttonAvailable(buttonIndex))
    return false;
  return pressedThisFrame[buttonIndex];
}

bool HalGPIO::wasReleased(uint8_t buttonIndex) const {
  if (buttonIndex >= NUM_BUTTONS || !buttonAvailable(buttonIndex))
    return false;
  return releasedThisFrame[buttonIndex];
}

bool HalGPIO::wasAnyPressed() const {
  for (int i = 0; i < NUM_BUTTONS; i++) {
    if (pressedThisFrame[i])
      return true;
  }
  return false;
}

bool HalGPIO::wasAnyReleased() const {
  for (int i = 0; i < NUM_BUTTONS; i++) {
    if (releasedThisFrame[i])
      return true;
  }
  return false;
}

unsigned long HalGPIO::getHeldTime() const {
  // Return the longest held time among all currently pressed buttons
  unsigned long now = SDL_GetTicks();
  unsigned long maxHeld = 0;
  const uint8_t *state = SDL_GetKeyboardState(NULL);
  for (int i = 0; i < NUM_BUTTONS; i++) {
    if (buttonAvailable(i) &&
        (state[buttonScancode[i]] || syntheticButtonDown[i] ||
         coverButtonDown[i]) &&
        buttonPressTime[i] > 0) {
      unsigned long held = now - buttonPressTime[i];
      if (held > maxHeld)
        maxHeld = held;
    }
  }
  return maxHeld;
}

unsigned long HalGPIO::getPowerButtonHeldTime() const {
  const uint8_t *state = SDL_GetKeyboardState(NULL);
  if ((!state[buttonScancode[BTN_POWER]] && !syntheticButtonDown[BTN_POWER]) ||
      buttonPressTime[BTN_POWER] == 0)
    return 0;
  return SDL_GetTicks() - buttonPressTime[BTN_POWER];
}

bool HalGPIO::wasCapacitivePagePressed() const {
  return capacitivePagePressedThisFrame;
}

bool HalGPIO::isCapacitivePagePressed(uint8_t buttonIndex) const {
  return buttonIndex < NUM_BUTTONS && coverButtonDown[buttonIndex];
}

bool HalGPIO::hasTouch() const { return BoardConfig::hasTouch(); }

bool HalGPIO::hasHomeKey() const { return BoardConfig::hasHomeKey(); }

bool HalGPIO::hasToggleSwitch() const {
  return BoardConfig::ACTIVE.input.toggleSwitch >= 0;
}

bool HalGPIO::isToggleSwitchOn() const {
  return hasToggleSwitch() && toggleSwitchOn;
}

bool HalGPIO::wasHomeKeyPressed() const { return homeKeyPressedThisFrame; }

bool HalGPIO::wasHomeKeyTapped() const { return homeKeyTappedThisFrame; }

bool HalGPIO::wasHomeKeyLongPressed() const {
  return homeKeyLongPressedThisFrame;
}

bool HalGPIO::wasTouchTap(float &nx, float &ny) const {
  if (!touchState.releasedThisFrame || touchState.movedBeyondTapSlop ||
      touchState.suppressed)
    return false;
  nx = touchState.startNx;
  ny = touchState.startNy;
  return true;
}

bool HalGPIO::wasTouchDown(float &nx, float &ny) const {
  if (!touchState.pressedThisFrame)
    return false;
  nx = touchState.startNx;
  ny = touchState.startNy;
  return true;
}

bool HalGPIO::wasTouchReleased() const { return touchState.releasedThisFrame; }

bool HalGPIO::wasTouchReleasedAt(float &nx, float &ny) const {
  if (!wasTouchReleased())
    return false;
  nx = touchState.currentNx;
  ny = touchState.currentNy;
  return true;
}

bool HalGPIO::isTouchTapCandidate(float &nx, float &ny,
                                  unsigned long &heldMs) const {
  if (!touchState.down || touchState.movedBeyondTapSlop ||
      touchState.suppressed) {
    heldMs = 0;
    return false;
  }
  nx = touchState.startNx;
  ny = touchState.startNy;
  heldMs = SDL_GetTicks() - touchState.pressedAt;
  return true;
}

bool HalGPIO::isTouchHeldAt(float &nx, float &ny) const {
  if (!touchState.down || touchState.suppressed)
    return false;
  nx = touchState.currentNx;
  ny = touchState.currentNy;
  return true;
}

bool HalGPIO::wasTouchLongPress(float &nx, float &ny) const {
  if (!touchState.longPressThisFrame || touchState.suppressed)
    return false;
  nx = touchState.startNx;
  ny = touchState.startNy;
  return true;
}

void HalGPIO::suppressTouchContact() {
  if (touchState.down || touchState.releasedThisFrame)
    touchState.suppressed = true;
}

unsigned long HalGPIO::lastTouchHeldMs() const { return touchState.lastHeldMs; }

bool HalGPIO::wasSwipe(float &nxStart, float &nyStart, float &nxEnd,
                       float &nyEnd) const {
  // Kindle: swipes in all directions, as in stock touch CrossPoint (page
  // turns, list scrolling, top-edge pull-down = light, bottom-edge swipe up
  // = reader menu). Taps work alongside them.
  if (!touchState.releasedThisFrame || touchState.suppressed ||
      touchState.lastHeldMs > TOUCH_SWIPE_MAX_MS)
    return false;
  const float dx =
      (touchState.currentNx - touchState.startNx) * kindle_geometry::get().width;
  const float dy =
      (touchState.currentNy - touchState.startNy) * kindle_geometry::get().height;
  if (std::abs(dx) < TOUCH_SWIPE_MIN_PX && std::abs(dy) < TOUCH_SWIPE_MIN_PX)
    return false;
  nxStart = touchState.startNx;
  nyStart = touchState.startNy;
  nxEnd = touchState.currentNx;
  nyEnd = touchState.currentNy;
  return true;
}

bool HalGPIO::wasTouchActivity() const { return touchState.activityThisFrame; }
void HalGPIO::setSharedConfirmPowerShortPressEmitsPower(bool /*enabled*/) {}

bool HalGPIO::consumeSimulatorSleepRequest() {
  const bool requested = simulatorSleepRequested;
  simulatorSleepRequested = false;
  return requested;
}

HalGPIO::WakeupReason HalGPIO::getWakeupReason() const {
  if (SimulatorLifecycle::consumeWakeReason() ==
      SimulatorLifecycle::WakeReason::PowerButton) {
    return WakeupReason::PowerButton;
  }
  return WakeupReason::Other;
}
bool HalGPIO::isUsbConnected() const { return true; }
bool HalGPIO::wasUsbStateChanged() const { return false; }

static void kindleRun(const char *cmd) {
  const int rc = std::system(cmd);
  std::fprintf(stderr, "[kindle] %s -> %d\n", cmd, rc);
}

// Time the system spent suspended so far: CLOCK_BOOTTIME keeps counting in
// suspend, CLOCK_MONOTONIC does not.
static double suspendedSeconds() {
  timespec boot{}, mono{};
  clock_gettime(CLOCK_BOOTTIME, &boot);
  clock_gettime(CLOCK_MONOTONIC, &mono);
  return (boot.tv_sec - mono.tv_sec) + (boot.tv_nsec - mono.tv_nsec) / 1e9;
}

// Cover (hall sensor): 1 = magnet present = cover closed. The PW3 exposes it
// as /sys/devices/system/wario_hall/wario_hall0/hall_detect; other models use
// the same name under their own board prefix or a platform device, so search.
static std::string hallPath() {
  static std::string path = [] {
    std::string found;
    if (const char *forced = std::getenv("CPK_HALL_PATH"); forced && *forced)
      return std::string(forced);  // tests
    // PW3: /sys/devices/system/wario_hall/wario_hall0; Scribe:
    // /sys/devices/platform/eink_hall (reads 0 with the cover open).
    for (const char *pattern : {"/sys/devices/system/*hall*/*hall*/hall_detect",
                                "/sys/devices/platform/*hall*/hall_detect"}) {
      glob_t g{};
      if (glob(pattern, 0, nullptr, &g) == 0 && g.gl_pathc > 0)
        found = g.gl_pathv[0];
      globfree(&g);
      if (!found.empty())
        break;
    }
    std::fprintf(stderr, "[kindle] cover sensor: %s\n",
                 found.empty() ? "not found" : found.c_str());
    return found;
  }();
  return path;
}

// -1 = no sensor, 0 = open, 1 = closed.
static int coverClosed() {
  const std::string &path = hallPath();
  if (path.empty())
    return -1;
  int value = -1;
  if (FILE *f = std::fopen(path.c_str(), "r")) {
    if (std::fscanf(f, "%d", &value) != 1)
      value = -1;
    std::fclose(f);
  }
  return value < 0 ? -1 : (value ? 1 : 0);
}

// Interrupt counts from /proc/interrupts, keyed by the line's name part.
// Comparing them before and after a suspend shows what woke the Kindle.
static std::vector<std::pair<std::string, long long>> readInterrupts() {
  std::vector<std::pair<std::string, long long>> out;
  FILE *f = std::fopen("/proc/interrupts", "r");
  if (!f)
    return out;
  char line[256];
  while (std::fgets(line, sizeof(line), f)) {
    char *colon = std::strchr(line, ':');
    if (!colon)
      continue;
    long long count = 0;
    char *rest = colon + 1;
    // Sum the per-CPU columns (numbers right after the colon).
    for (;;) {
      char *end = nullptr;
      const long long v = std::strtoll(rest, &end, 10);
      if (end == rest)
        break;
      count += v;
      rest = end;
    }
    std::string name(line, colon - line);
    std::string tail(rest);
    while (!tail.empty() && (tail.back() == '\n' || tail.back() == ' '))
      tail.pop_back();
    size_t start = tail.find_first_not_of(' ');
    name += start == std::string::npos ? "" : " " + tail.substr(start);
    out.emplace_back(name, count);
  }
  std::fclose(f);
  return out;
}

static void logInterruptChanges(const std::vector<std::pair<std::string, long long>> &before) {
  const auto after = readInterrupts();
  std::string changed;
  for (const auto &a : after) {
    for (const auto &b : before) {
      if (a.first == b.first && a.second != b.second) {
        // Skip the always-busy timer lines.
        if (a.first.find("timer") != std::string::npos ||
            a.first.find("Timer") != std::string::npos)
          break;
        char buf[160];
        std::snprintf(buf, sizeof(buf), "\n    %s +%lld", a.first.c_str(), a.second - b.second);
        changed += buf;
        break;
      }
    }
  }
  std::fprintf(stderr, "[kindle] interrupts during suspend:%s\n",
               changed.empty() ? " (none changed)" : changed.c_str());
}

// Suspend the SoC directly: write "mem" to /sys/power/state. Blocks until a
// wake source (power button, cover) resumes the system. Returns the seconds
// spent suspended, or a negative value if the kernel refused.
static double suspendOnce() {
  const char *statePath = std::getenv("CPK_POWER_STATE_PATH");  // tests
  if (!statePath || !*statePath)
    statePath = "/sys/power/state";
  const double before = suspendedSeconds();
  kindlePowerLogNote("going to sleep");
  const auto irqBefore = readInterrupts();
  FILE *f = std::fopen(statePath, "w");
  if (!f) {
    std::fprintf(stderr, "[kindle] cannot open %s: %s\n", statePath, std::strerror(errno));
    return -1;
  }
  const bool ok = std::fputs("mem", f) >= 0 && std::fflush(f) == 0;
  const int err = errno;
  std::fclose(f);
  const double slept = suspendedSeconds() - before;
  if (!ok) {
    std::fprintf(stderr, "[kindle] suspend refused: %s\n", std::strerror(err));
    return -1;
  }
  logInterruptChanges(irqBefore);
  kindlePowerLogNote("woke up");
  return slept;
}

// The firmware has drawn its sleep screen. Put the Kindle to sleep and block
// until the user wakes it with the power button or by opening the cover.
//
// powerd is not involved: with the Kindle UI paused it never leaves Active
// (device logs), so CrossPoint suspends the kernel itself, the way KOReader
// does on Kobo. preventScreenSaver stays set so powerd keeps out of the way.
// If the kernel will not suspend, the sleep screen stays up ("soft sleep")
// and the first power press or cover opening wakes it. No input is ignored
// except what was queued before the sleep started.
static void kindleSleepUntilWake() {
  std::fprintf(stderr, "[kindle] going to sleep\n");
  // Let the panel finish drawing the sleep screen before the EPDC loses
  // power, then drop the press (and release) that started the sleep.
  delay(1500);
  kindle_input::waitForPowerPress(0);

  int spurious = 0;
  for (;;) {
    if (quitRequested.load())
      return;
    const double slept = suspendOnce();
    if (slept < 0.5) {
      // Refused or returned at once: soft sleep, wait for a press or the cover.
      std::fprintf(stderr, "[kindle] no suspend (%.1f s); waiting on the sleep screen\n",
                   slept < 0 ? 0.0 : slept);
      int lastCover = coverClosed();
      for (;;) {
        if (quitRequested.load())
          return;
        if (kindle_input::waitForPowerPress(250)) {
          std::fprintf(stderr, "[kindle] woken (power key)\n");
          return;
        }
        const int cover = coverClosed();
        if (lastCover == 1 && cover == 0) {
          std::fprintf(stderr, "[kindle] woken (cover opened)\n");
          return;
        }
        lastCover = cover;
      }
    }
    // Resumed after a real suspend. Find out why.
    const bool key = kindle_input::waitForPowerPress(300);
    const int cover = coverClosed();
    std::fprintf(stderr, "[kindle] resumed after %.0f s suspended (key=%d cover=%d)\n",
                 slept, key ? 1 : 0, cover);
    // A wake with the cover still closed and no power press is spurious
    // (e.g. a timer): go back to sleep. Anything else is the user.
    if (!key && cover == 1 && ++spurious < 50)
      continue;
    std::fprintf(stderr, "[kindle] woken (%s)\n",
                 key ? "power key" : cover == 0 ? "cover open" : "resume");
    return;
  }
}

// Cover closed while CrossPoint is running: same as a power press (sleep).
// Called from HalGPIO::update(); reads the sensor twice a second.
static void pollCover() {
  static unsigned long lastRead = 0;
  static int last = -2;
  if (millis() - lastRead < 500)
    return;
  lastRead = millis();
  const int now = coverClosed();
  if (last == 0 && now == 1) {
    std::fprintf(stderr, "[kindle] cover closed: sleeping\n");
    pressedThisFrame[HalGPIO::BTN_POWER] = true;
    syntheticButtonDown[HalGPIO::BTN_POWER] = true;
    buttonPressTime[HalGPIO::BTN_POWER] = std::max<uint32_t>(1, SDL_GetTicks() - 600);
    coverPressPending = true;
    coverPressFrame = frameNo;
  }
  last = now;
}

void HalGPIO::startDeepSleep() {
  clearButtonState();
  if (std::getenv("CPK_FAKE_FB") && !std::getenv("CPK_TEST_SLEEP")) {
    // PC test runs have no powerd: behave like the old exit.
    std::fprintf(stderr, "[kindle] sleep requested (test run); exiting\n");
    quitRequested.store(true);
    return;
  }
  kindleSleepUntilWake();
  if (quitRequested.load())
    return;
  // powerd's idle timer kept running while we slept; restart it.
  kindleRun("lipc-set-prop com.lab126.powerd preventScreenSaver 1 >/dev/null 2>&1");
  // An ESP32 wakes from deep sleep by rebooting, and the firmware expects
  // exactly that (it reopens the book). Restart this process the same way.
  setenv("CPK_JUST_WOKE", "1", 1);
  SimulatorLifecycle::rebootAsPowerWake();
}

bool HalGPIO::verifyPowerButtonWakeup() { return true; }

HalGPIO gpio;
