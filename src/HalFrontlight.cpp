#include "HalFrontlight.h"

#include <BoardConfig.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iostream>

namespace {
// Kindle: powerd owns the frontlight. Its flIntensity range is 0..24 on the
// Paperwhite 3. Runs lipc-set-prop in the background so a slider drag never
// stalls the UI; skipped under CPK_FAKE_FB (PC test runs).
void applyKindleFrontlight(bool on, uint8_t percent) {
  static int lastLevel = -1;
  const int level = on ? std::max(1, (percent * 24 + 50) / 100) : 0;
  if (level == lastLevel || std::getenv("CPK_FAKE_FB"))
    return;
  lastLevel = level;
  char cmd[96];
  std::snprintf(cmd, sizeof(cmd),
                "lipc-set-prop com.lab126.powerd flIntensity %d >/dev/null 2>&1 &",
                level);
  if (std::system(cmd) != 0)
    std::cerr << "[kindle] frontlight command failed" << std::endl;
}
}  // namespace

HalFrontlight &HalFrontlight::getInstance() {
  static HalFrontlight instance;
  return instance;
}

void HalFrontlight::begin(uint8_t brightness, uint8_t warmth, bool on) {
  if (!present())
    return;
  lastBrightness = std::min<uint8_t>(brightness, 100);
  lastWarmth = hasColorTemperature() ? std::min<uint8_t>(warmth, 100) : 0;
  lit = on;
  applyKindleFrontlight(lit, lastBrightness);
  std::cerr << "[kindle] Frontlight: " << (lit ? "on" : "off")
            << ", brightness=" << static_cast<unsigned>(lastBrightness)
            << "%, warmth=" << static_cast<unsigned>(lastWarmth) << "%"
            << std::endl;
}

bool HalFrontlight::present() const {
#if !defined(SIMULATOR_EEGO_NO_FRONTLIGHT)
  if (BoardConfig::hasI2cFrontlight())
    return true;
#endif
  return BoardConfig::hasPwmFrontlight();
}

bool HalFrontlight::hasColorTemperature() const {
  // The Paperwhite 3 light has no colour temperature control.
  return BoardConfig::isX4Pro() ||
         ((BoardConfig::isEegoA4() || BoardConfig::isPicco()) && present());
}

void HalFrontlight::setBrightness(uint8_t percent) {
  lastBrightness = std::min<uint8_t>(percent, 100);
  if (lit)
    applyKindleFrontlight(lit, lastBrightness);
}

void HalFrontlight::setWarmth(uint8_t warmPercent) {
  if (hasColorTemperature())
    lastWarmth = std::min<uint8_t>(warmPercent, 100);
}

void HalFrontlight::setOn(bool on) {
  lit = present() && on;
  applyKindleFrontlight(lit, lastBrightness);
}

uint8_t HalFrontlight::brightness() const { return lastBrightness; }

uint8_t HalFrontlight::warmth() const { return lastWarmth; }

bool HalFrontlight::isOn() const { return lit; }
