#include "KindleInput.h"

#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <poll.h>
#include <sys/select.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace kindle_input {
namespace {

constexpr int kMaxSlots = 10;
constexpr unsigned long kHomeTapMaxMs = 600;
constexpr float kMultiTapSlop = 0.04f;  // fraction of the panel

struct Slot {
  bool active = false;
  int x = 0, y = 0;
  float startU = 0, startV = 0;
};

int fd = -1;
int keyFd = -1;  // power button (not grabbed: powerd shares it)
input_absinfo absX{}, absY{};
bool multiTouch = false;
Orientation orient;

Slot slots[kMaxSlots];
int currentSlot = 0;
bool stPressed = false;  // single-touch BTN_TOUCH state

// Gesture state, evaluated on each SYN_REPORT.
int prevCount = 0;
int primarySlot = -1;
bool multi = false;          // a second finger joined this contact
int multiMaxCount = 0;
bool multiMoved = false;
unsigned long multiStartMs = 0;

unsigned long nowMs() {
  using namespace std::chrono;
  return static_cast<unsigned long>(
      duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

bool testBit(const unsigned long* bits, int bit) {
  constexpr int kBitsPerLong = sizeof(unsigned long) * 8;
  return (bits[bit / kBitsPerLong] >> (bit % kBitsPerLong)) & 1UL;
}

bool envFlag(const char* name, bool fallback) {
  const char* v = std::getenv(name);
  if (!v || !*v) return fallback;
  return v[0] == '1' || v[0] == 'y' || v[0] == 'Y' || v[0] == 't' || v[0] == 'T';
}

void normalize(int rawX, int rawY, float& u, float& v) {
  auto norm = [](int value, const input_absinfo& a) {
    const int span = std::max(1, a.maximum - a.minimum);
    return std::clamp(static_cast<float>(value - a.minimum) / span, 0.0f, 1.0f);
  };
  float nx = norm(rawX, absX);
  float ny = norm(rawY, absY);
  if (orient.swapAxes) std::swap(nx, ny);
  if (orient.mirrorX) nx = 1.0f - nx;
  if (orient.mirrorY) ny = 1.0f - ny;
  u = nx;
  v = ny;
}

int activeCount() {
  int n = 0;
  for (const Slot& s : slots) n += s.active ? 1 : 0;
  return n;
}

int firstActive() {
  for (int i = 0; i < kMaxSlots; ++i)
    if (slots[i].active) return i;
  return -1;
}

void onFrame(std::vector<Event>& out) {
  const int count = activeCount();

  // Remember where each new finger started (for the multi-finger tap test).
  for (Slot& s : slots) {
    if (s.active && s.startU < 0) normalize(s.x, s.y, s.startU, s.startV);
  }

  if (prevCount == 0 && count > 0) {
    primarySlot = firstActive();
    multi = count > 1;
    multiMaxCount = count;
    multiMoved = false;
    multiStartMs = nowMs();
    if (!multi) {
      Event e{Event::Type::Down};
      normalize(slots[primarySlot].x, slots[primarySlot].y, e.u, e.v);
      out.push_back(e);
    }
  } else if (count > 0) {
    if (count > 1 && !multi) {
      // A second finger arrived: this is a gesture, not a page tap.
      multi = true;
      multiStartMs = nowMs();
      out.push_back({Event::Type::Cancel});
    }
    multiMaxCount = std::max(multiMaxCount, count);
    if (!multi) {
      if (primarySlot < 0 || !slots[primarySlot].active) primarySlot = firstActive();
      Event e{Event::Type::Move};
      normalize(slots[primarySlot].x, slots[primarySlot].y, e.u, e.v);
      out.push_back(e);
    } else {
      for (const Slot& s : slots) {
        if (!s.active) continue;
        float u, v;
        normalize(s.x, s.y, u, v);
        if (std::fabs(u - s.startU) > kMultiTapSlop || std::fabs(v - s.startV) > kMultiTapSlop)
          multiMoved = true;
      }
    }
  } else if (prevCount > 0 && count == 0) {
    if (multi) {
      // A quick two-finger tap; longer holds do nothing.
      if (!multiMoved && multiMaxCount == 2 && nowMs() - multiStartMs <= kHomeTapMaxMs)
        out.push_back({Event::Type::HomeTap});
    } else {
      Event e{Event::Type::Up};
      if (primarySlot >= 0) normalize(slots[primarySlot].x, slots[primarySlot].y, e.u, e.v);
      out.push_back(e);
    }
    primarySlot = -1;
  }
  prevCount = count;
}

void handle(const input_event& ev, std::vector<Event>& out) {
  if (ev.type == EV_ABS) {
    switch (ev.code) {
      case ABS_MT_SLOT:
        currentSlot = std::clamp(ev.value, 0, kMaxSlots - 1);
        break;
      case ABS_MT_TRACKING_ID:
        if (ev.value < 0) {
          slots[currentSlot].active = false;
        } else if (!slots[currentSlot].active) {
          slots[currentSlot].active = true;
          slots[currentSlot].startU = -1;  // filled on the next frame
        }
        break;
      case ABS_MT_POSITION_X:
        slots[currentSlot].x = ev.value;
        break;
      case ABS_MT_POSITION_Y:
        slots[currentSlot].y = ev.value;
        break;
      case ABS_X:
        if (!multiTouch) slots[0].x = ev.value;
        break;
      case ABS_Y:
        if (!multiTouch) slots[0].y = ev.value;
        break;
      default:
        break;
    }
  } else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
    stPressed = ev.value != 0;
    if (!multiTouch) {
      slots[0].active = stPressed;
      if (stPressed) slots[0].startU = -1;
    } else if (!stPressed) {
      // Some kernels only report the final lift through BTN_TOUCH.
      for (Slot& s : slots) s.active = false;
    }
  } else if (ev.type == EV_SYN) {
    if (ev.code == SYN_REPORT) {
      onFrame(out);
    } else if (ev.code == SYN_DROPPED) {
      for (Slot& s : slots) s.active = false;
      if (prevCount > 0) out.push_back({Event::Type::Cancel});
      prevCount = 0;
      primarySlot = -1;
    }
  }
}

// Kindle: the touchscreen is the first multitouch device; failing that, the
// first single-touch device with BTN_TOUCH. Other devices with ABS_X/ABS_Y
// are skipped: the Scribe's accelerometer (kx132-accel, no keys) and its pen
// digitizers (BTN_TOOL_PEN) come before the touchscreen. CPK_TOUCH_DEVICE
// forces a path.
enum class DeviceKind { Other, MultiTouch, SingleTouch };

DeviceKind classifyDevice(const unsigned long* absBits, const unsigned long* keyBits) {
  if (testBit(absBits, ABS_MT_POSITION_X) && testBit(absBits, ABS_MT_POSITION_Y))
    return DeviceKind::MultiTouch;
  if (testBit(absBits, ABS_X) && testBit(absBits, ABS_Y) && testBit(keyBits, BTN_TOUCH) &&
      !testBit(keyBits, BTN_TOOL_PEN))
    return DeviceKind::SingleTouch;
  return DeviceKind::Other;
}

int openTouchDevice() {
  const char* forced = std::getenv("CPK_TOUCH_DEVICE");
  const bool isForced = forced && *forced;
  for (int pass = 0; pass < 2; ++pass) {
    for (int i = 0; i < 16; ++i) {
      const std::string path = isForced ? std::string(forced) : "/dev/input/event" + std::to_string(i);
      const int f = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
      if (f < 0) {
        if (isForced) break;
        continue;
      }
      unsigned long absBits[(ABS_MAX + 1) / (sizeof(unsigned long) * 8) + 1] = {};
      unsigned long keyBits[(KEY_MAX + 1) / (sizeof(unsigned long) * 8) + 1] = {};
      const bool haveAbs = ioctl(f, EVIOCGBIT(EV_ABS, sizeof(absBits)), absBits) >= 0;
      ioctl(f, EVIOCGBIT(EV_KEY, sizeof(keyBits)), keyBits);
      const DeviceKind kind = haveAbs ? classifyDevice(absBits, keyBits) : DeviceKind::Other;
      const bool mt = kind == DeviceKind::MultiTouch;
      const bool anyXY = haveAbs && testBit(absBits, ABS_X) && testBit(absBits, ABS_Y);
      if (isForced ? (mt || anyXY)
                   : kind == (pass == 0 ? DeviceKind::MultiTouch : DeviceKind::SingleTouch)) {
        multiTouch = mt;
        ioctl(f, EVIOCGABS(mt ? ABS_MT_POSITION_X : ABS_X), &absX);
        ioctl(f, EVIOCGABS(mt ? ABS_MT_POSITION_Y : ABS_Y), &absY);
        char name[128] = "?";
        ioctl(f, EVIOCGNAME(sizeof(name)), name);
        std::fprintf(stderr, "[kindle] touch: %s (%s) %s x=%d..%d y=%d..%d\n", path.c_str(),
                     name, mt ? "multitouch" : "single-touch", absX.minimum, absX.maximum,
                     absY.minimum, absY.maximum);
        return f;
      }
      close(f);
      if (isForced) break;
    }
    if (isForced) break;
  }
  return -1;
}

// Kindle: pen digitizers (Scribe). The Kindle's ink handler (WetInkHandler)
// reads the pen and draws strokes straight into the framebuffer, over
// CrossPoint. Grabbing the pen devices keeps their events from it (and from
// Xorg) while CrossPoint runs; the pen is not used as input yet.
std::vector<int> penFds;

void grabPens() {
  for (int i = 0; i < 16; ++i) {
    const std::string path = "/dev/input/event" + std::to_string(i);
    const int f = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (f < 0) continue;
    struct stat a{}, b{};
    if (fd >= 0 && fstat(f, &a) == 0 && fstat(fd, &b) == 0 && a.st_rdev == b.st_rdev) {
      close(f);  // never the touchscreen
      continue;
    }
    unsigned long absBits[(ABS_MAX + 1) / (sizeof(unsigned long) * 8) + 1] = {};
    unsigned long keyBits[(KEY_MAX + 1) / (sizeof(unsigned long) * 8) + 1] = {};
    ioctl(f, EVIOCGBIT(EV_ABS, sizeof(absBits)), absBits);
    if (ioctl(f, EVIOCGBIT(EV_KEY, sizeof(keyBits)), keyBits) >= 0 &&
        testBit(keyBits, BTN_TOOL_PEN) && !testBit(absBits, ABS_MT_POSITION_X)) {
      char name[128] = "?";
      ioctl(f, EVIOCGNAME(sizeof(name)), name);
      if (ioctl(f, EVIOCGRAB, 1) == 0) {
        std::fprintf(stderr, "[kindle] pen: %s (%s) held while CrossPoint runs\n", path.c_str(), name);
        penFds.push_back(f);
        continue;
      }
      std::fprintf(stderr, "[kindle] pen: %s (%s) could not be held: %s\n", path.c_str(), name,
                   std::strerror(errno));
    }
    close(f);
  }
}

void releasePens() {
  for (int f : penFds) {
    ioctl(f, EVIOCGRAB, 0);
    close(f);
  }
  penFds.clear();
}

int openPowerKeyDevice() {
  const char* forced = std::getenv("CPK_POWER_DEVICE");
  for (int i = 0; i < 16; ++i) {
    const std::string path = forced && *forced ? std::string(forced)
                                               : "/dev/input/event" + std::to_string(i);
    const int f = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (f >= 0) {
      unsigned long keyBits[(KEY_MAX + 1) / (sizeof(unsigned long) * 8) + 1] = {};
      if (ioctl(f, EVIOCGBIT(EV_KEY, sizeof(keyBits)), keyBits) >= 0 &&
          testBit(keyBits, KEY_POWER)) {
        char name[128] = "?";
        ioctl(f, EVIOCGNAME(sizeof(name)), name);
        std::fprintf(stderr, "[kindle] power key: %s (%s)\n", path.c_str(), name);
        return f;
      }
      close(f);
    }
    if (forced && *forced) break;
  }
  std::fprintf(stderr, "[kindle] no power key device found\n");
  return -1;
}

void pollPowerKey(std::vector<Event>& out) {
  if (keyFd < 0) return;
  input_event evs[16];
  for (;;) {
    const ssize_t n = read(keyFd, evs, sizeof(evs));
    if (n <= 0) break;
    for (size_t i = 0; i < static_cast<size_t>(n) / sizeof(input_event); ++i) {
      const input_event& e = evs[i];
      if (e.type != EV_KEY || e.code != KEY_POWER) continue;
      // Logged so a missed press shows up in crosspoint.log.
      std::fprintf(stderr, "[kindle] power key %s\n",
                   e.value == 1 ? "down" : e.value == 0 ? "up" : "repeat");
      if (e.value == 1) out.push_back({Event::Type::PowerDown});
      if (e.value == 0) out.push_back({Event::Type::PowerUp});
    }
  }
}

}  // namespace

bool waitForPowerPress(int timeoutMs) {
  if (keyFd < 0) {
    usleep(static_cast<useconds_t>(timeoutMs) * 1000);
    return false;
  }
  fd_set set;
  FD_ZERO(&set);
  FD_SET(keyFd, &set);
  timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
  if (select(keyFd + 1, &set, nullptr, nullptr, &tv) <= 0) return false;
  std::vector<Event> evs;
  pollPowerKey(evs);
  // Drop touches that arrived while asleep.
  if (fd >= 0) {
    input_event junk[64];
    while (read(fd, junk, sizeof(junk)) > 0) {
    }
  }
  for (const Event& e : evs)
    if (e.type == Event::Type::PowerDown) return true;
  return false;
}

void waitForInput(int timeoutMs) {
  pollfd fds[2];
  int n = 0;
  if (fd >= 0) fds[n++] = {fd, POLLIN, 0};
  if (keyFd >= 0) fds[n++] = {keyFd, POLLIN, 0};
  if (n == 0) {
    usleep(static_cast<useconds_t>(timeoutMs) * 1000);
    return;
  }
  ::poll(fds, n, timeoutMs);
}

bool begin(Orientation defaults) {
  orient.swapAxes = envFlag("CPK_TOUCH_SWAP_XY", defaults.swapAxes);
  orient.mirrorX = envFlag("CPK_TOUCH_MIRROR_X", defaults.mirrorX);
  orient.mirrorY = envFlag("CPK_TOUCH_MIRROR_Y", defaults.mirrorY);
  keyFd = openPowerKeyDevice();
  if (keyFd >= 0) {
    // If another program holds the power key exclusively, its presses never
    // reach us. Find out by grabbing it for a moment (and letting go).
    if (ioctl(keyFd, EVIOCGRAB, 1) < 0) {
      std::fprintf(stderr, "[kindle] power key is held by another program (%s)\n",
                   std::strerror(errno));
    } else {
      ioctl(keyFd, EVIOCGRAB, 0);
      std::fprintf(stderr, "[kindle] power key is free (no other program grabs it)\n");
    }
  }
  fd = openTouchDevice();
  if (fd < 0) {
    std::fprintf(stderr, "[kindle] no touchscreen found\n");
    return false;
  }
  // Keep touches away from the paused Kindle UI; it would replay them later.
  if (ioctl(fd, EVIOCGRAB, 1) < 0) std::perror("[kindle] EVIOCGRAB");
  grabPens();
  std::fprintf(stderr, "[kindle] touch transform: swap=%d mirrorX=%d mirrorY=%d\n",
               orient.swapAxes, orient.mirrorX, orient.mirrorY);
  return true;
}

void poll(std::vector<Event>& out) {
  pollPowerKey(out);
  // Tests: CPK_TEST_POWER_TAP_MS=<ms> delivers one quick power tap (press and
  // release in the same poll) that many ms after start.
  static long tapAt = [] {
    const char* v = std::getenv("CPK_TEST_POWER_TAP_MS");
    return v && *v ? std::atol(v) : -1L;
  }();
  static const long long startMs = nowMs();
  if (tapAt >= 0 && nowMs() - startMs >= tapAt) {
    tapAt = -1;
    std::fprintf(stderr, "[kindle] test: power tap\n");
    out.push_back({Event::Type::PowerDown});
    out.push_back({Event::Type::PowerUp});
  }
  if (fd < 0) return;
  input_event evs[64];
  for (;;) {
    const ssize_t n = read(fd, evs, sizeof(evs));
    if (n <= 0) break;
    const size_t count = static_cast<size_t>(n) / sizeof(input_event);
    for (size_t i = 0; i < count; ++i) handle(evs[i], out);
  }
}

void end() {
  releasePens();
  if (keyFd >= 0) {
    close(keyFd);
    keyFd = -1;
  }
  if (fd >= 0) {
    ioctl(fd, EVIOCGRAB, 0);
    close(fd);
    fd = -1;
  }
}

}  // namespace kindle_input
