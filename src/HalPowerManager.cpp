#include "HalPowerManager.h"

#include <Arduino.h>

#include <cstdio>

#include "HalGPIO.h"
#include "SimulatorLifecycle.h"

HalPowerManager powerManager;

void HalPowerManager::begin() {}
void HalPowerManager::startDeepSleep(HalGPIO &gpio,
                                     bool wakeOnHeldButton) const {
  if (wakeOnHeldButton)
    SimulatorLifecycle::rebootAsPowerWake();
  gpio.startDeepSleep();
}
#if FREEINK_DEVICE_PICCO
bool HalPowerManager::holdWhileUsbPowered(
    HalGPIO & /*gpio*/, void (* /*onStatus*/)(bool, uint16_t)) const {
  // No charger model: keep the existing host sleep/wake path available.
  return false;
}
#endif
void HalPowerManager::setPowerSaving(bool enable) {}
// Kindle: read the gas gauge from sysfs (cached for 30 s; the status bar asks
// every redraw). PW3 is a "wario" board; the Scribe (MediaTek) uses
// bd71827_bat; the other paths cover other models.
uint16_t HalPowerManager::getBatteryPercentage() const {
  static const char *const kPaths[] = {
      "/sys/devices/system/wario_battery/wario_battery0/battery_capacity",
      "/sys/devices/system/yoshi_battery/yoshi_battery0/battery_capacity",
      "/sys/devices/system/luigi_battery/luigi_battery0/battery_capacity",
      "/sys/class/power_supply/bd7181x_bat/capacity",
      "/sys/class/power_supply/bd71827_bat/capacity",
      "/sys/class/power_supply/max77796-battery/capacity",
  };
  static uint16_t cached = 100;
  static unsigned long lastRead = 0;
  static bool haveRead = false;
  if (haveRead && millis() - lastRead < 30000)
    return cached;
  haveRead = true;
  lastRead = millis();
  for (const char *path : kPaths) {
    if (FILE *f = std::fopen(path, "r")) {
      int value = -1;
      // Some kernels print "85%", others "85".
      const int n = std::fscanf(f, "%d", &value);
      std::fclose(f);
      if (n == 1 && value >= 0 && value <= 100) {
        cached = static_cast<uint16_t>(value);
        break;
      }
    }
  }
  return cached;
}

HalPowerManager::Lock::Lock() {}
HalPowerManager::Lock::~Lock() {}
