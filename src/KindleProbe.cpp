// Kindle: `crosspoint --probe` prints what this Kindle looks like to the port
// (FBInk's device identification and framebuffer, the input devices and
// their ranges) and exits. Read-only: it never draws, refreshes or grabs an
// input device, so it is safe with the Kindle UI running. Used by
// kual/crosspoint/bin/probe.sh to collect facts when adding a new model.

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "fbink.h"

namespace {

bool testBit(const unsigned long* bits, int bit) {
  const int per = static_cast<int>(sizeof(unsigned long) * 8);
  return (bits[bit / per] >> (bit % per)) & 1UL;
}

void probeFramebuffer() {
  std::printf("== FBInk\n");
  const int fd = fbink_open();
  if (fd < 0) {
    std::printf("fbink_open failed\n");
    return;
  }
  FBInkConfig cfg{};
  cfg.is_quiet = true;
  if (fbink_init(fd, &cfg) < 0) {
    std::printf("fbink_init failed\n");
    fbink_close(fd);
    return;
  }
  FBInkState s{};
  fbink_get_state(&cfg, &s);
  std::printf("device_name=%s\n", s.device_name);
  std::printf("device_codename=%s\n", s.device_codename);
  std::printf("device_platform=%s\n", s.device_platform);
  std::printf("device_id=%u (0x%X)\n", static_cast<unsigned>(s.device_id),
              static_cast<unsigned>(s.device_id));
  std::printf("screen=%ux%u view=%ux%u dpi=%u\n", s.screen_width, s.screen_height,
              s.view_width, s.view_height, s.screen_dpi);
  std::printf("stride=%u bpp=%u pixel_format=%d inverted=%d\n", s.scanline_stride, s.bpp,
              static_cast<int>(s.pixel_format), s.inverted_grayscale ? 1 : 0);
  std::printf("current_rota=%u can_rotate=%d is_mtk=%d legacy_einkfb=%d color=%d\n",
              s.current_rota, s.can_rotate ? 1 : 0, s.is_mtk ? 1 : 0,
              s.is_kindle_legacy ? 1 : 0, s.has_color_panel ? 1 : 0);
  std::printf("touch_swap_axes=%d touch_mirror_x=%d touch_mirror_y=%d\n",
              s.touch_swap_axes ? 1 : 0, s.touch_mirror_x ? 1 : 0, s.touch_mirror_y ? 1 : 0);
  std::printf("can_wake_epdc=%d unreliable_wait_for=%d can_wait_for_submission=%d "
              "eclipse_wfm=%d hw_invert=%d\n",
              s.can_wake_epdc ? 1 : 0, s.unreliable_wait_for ? 1 : 0,
              s.can_wait_for_submission ? 1 : 0, s.has_eclipse_wfm ? 1 : 0,
              s.can_hw_invert ? 1 : 0);
  fbink_close(fd);
}

void printAbs(int fd, int code, const char* name) {
  input_absinfo a{};
  if (ioctl(fd, EVIOCGABS(code), &a) == 0)
    std::printf("  %s: %d..%d res=%d\n", name, a.minimum, a.maximum, a.resolution);
}

void probeInput() {
  std::printf("== Input devices\n");
  std::vector<std::string> names;
  if (DIR* d = opendir("/dev/input")) {
    while (dirent* e = readdir(d))
      if (std::strncmp(e->d_name, "event", 5) == 0) names.push_back(e->d_name);
    closedir(d);
  }
  std::sort(names.begin(), names.end());
  for (const std::string& n : names) {
    const std::string path = "/dev/input/" + n;
    const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
      std::printf("%s: cannot open\n", path.c_str());
      continue;
    }
    char name[128] = {0};
    ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);
    input_id id{};
    ioctl(fd, EVIOCGID, &id);
    std::printf("%s: \"%s\" bus=0x%x vendor=0x%x product=0x%x\n", path.c_str(), name,
                id.bustype, id.vendor, id.product);

    unsigned long ev[(EV_MAX + 1 + 8 * sizeof(long) - 1) / (8 * sizeof(long))] = {0};
    unsigned long abs[(ABS_MAX + 1 + 8 * sizeof(long) - 1) / (8 * sizeof(long))] = {0};
    unsigned long key[(KEY_MAX + 1 + 8 * sizeof(long) - 1) / (8 * sizeof(long))] = {0};
    ioctl(fd, EVIOCGBIT(0, sizeof(ev)), ev);
    std::printf("  events:%s%s%s%s\n", testBit(ev, EV_KEY) ? " KEY" : "",
                testBit(ev, EV_ABS) ? " ABS" : "", testBit(ev, EV_SW) ? " SW" : "",
                testBit(ev, EV_MSC) ? " MSC" : "");
    if (testBit(ev, EV_ABS)) {
      ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs)), abs);
      if (testBit(abs, ABS_X)) printAbs(fd, ABS_X, "ABS_X");
      if (testBit(abs, ABS_Y)) printAbs(fd, ABS_Y, "ABS_Y");
      if (testBit(abs, ABS_PRESSURE)) printAbs(fd, ABS_PRESSURE, "ABS_PRESSURE");
      if (testBit(abs, ABS_DISTANCE)) printAbs(fd, ABS_DISTANCE, "ABS_DISTANCE");
      if (testBit(abs, ABS_TILT_X)) printAbs(fd, ABS_TILT_X, "ABS_TILT_X");
      if (testBit(abs, ABS_MT_SLOT)) printAbs(fd, ABS_MT_SLOT, "ABS_MT_SLOT");
      if (testBit(abs, ABS_MT_POSITION_X)) printAbs(fd, ABS_MT_POSITION_X, "ABS_MT_POSITION_X");
      if (testBit(abs, ABS_MT_POSITION_Y)) printAbs(fd, ABS_MT_POSITION_Y, "ABS_MT_POSITION_Y");
      if (testBit(abs, ABS_MT_TOOL_TYPE)) printAbs(fd, ABS_MT_TOOL_TYPE, "ABS_MT_TOOL_TYPE");
    }
    if (testBit(ev, EV_KEY)) {
      ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key)), key);
      std::printf("  keys:");
      int shown = 0;
      for (int k = 0; k <= KEY_MAX && shown < 24; ++k)
        if (testBit(key, k)) {
          std::printf(" %d", k);
          ++shown;
        }
      std::printf("%s\n", shown == 24 ? " ..." : "");
    }
    close(fd);
  }
}

}  // namespace

int kindleProbe() {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  probeFramebuffer();
  probeInput();
  return 0;
}
