#pragma once
// Kindle networking helpers. The Kindle OS owns Wi-Fi (join networks from the
// Kindle's own settings); CrossPoint needs the address it got (ipv4) and
// switches the radio on and off around its network screens (setRadio; see
// RadioState below and kual/crosspoint/bin/crosspoint.sh).

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace kindle_net {

// IPv4 address of wlan0 (or, failing that, any other non-loopback interface
// that is up, e.g. usb0 with USBNetwork). Returns 0 when there is none.
// Host byte order.
inline uint32_t ipv4() {
  ifaddrs* list = nullptr;
  if (getifaddrs(&list) != 0) return 0;
  uint32_t wlan = 0, other = 0;
  for (ifaddrs* it = list; it; it = it->ifa_next) {
    if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET) continue;
    if (!(it->ifa_flags & IFF_UP) || (it->ifa_flags & IFF_LOOPBACK)) continue;
    const uint32_t addr = ntohl(reinterpret_cast<sockaddr_in*>(it->ifa_addr)->sin_addr.s_addr);
    if (std::strcmp(it->ifa_name, "wlan0") == 0) {
      wlan = addr;
    } else if (!other) {
      other = addr;
    }
  }
  freeifaddrs(list);
  return wlan ? wlan : other;
}

// Wi-Fi radio. The launcher turns the radio off while CrossPoint runs (it
// draws power even when idle) and restores it on exit. CrossPoint turns it
// on when a network screen needs it (WiFi.mode/begin) and off again when the
// firmware is done with it (WiFi.mode(WIFI_OFF), sleep). CPK_KEEP_WIFI=1
// leaves the radio alone.
struct RadioState {
  bool managed = false;  // false: never touch the radio
  bool on = false;
  long long onAtMs = 0;
};

inline long long nowMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

inline RadioState& radio() {
  static RadioState state = [] {
    RadioState r;
    const char* keep = std::getenv("CPK_KEEP_WIFI");
    const char* off = std::getenv("CPK_WIFI_WAS_TURNED_OFF");  // set by the launcher
    r.managed = !(keep && *keep == '1') && off && *off == '1';
    // Managed: the real state is unknown at start (a restart after a wake
    // may follow a network session), so treat it as on; HalGPIO::begin
    // turns it off.
    r.on = true;
    return r;
  }();
  return state;
}

inline void setRadio(bool on) {
  RadioState& r = radio();
  if (!r.managed || r.on == on) return;
  r.on = on;
  if (on) r.onAtMs = nowMs();
  const char* cmd = on ? "lipc-set-prop com.lab126.cmd wirelessEnable 1 >/dev/null 2>&1"
                       : "lipc-set-prop com.lab126.cmd wirelessEnable 0 >/dev/null 2>&1";
  const int rc = std::system(cmd);
  std::fprintf(stderr, "[kindle] Wi-Fi radio %s (%d)\n", on ? "on" : "off", rc);
}

inline bool radioOn() { return radio().on; }

// Address, or 0 while a test asks to pretend the Kindle is still rejoining
// (CPK_TEST_IP_DELAY_MS after the radio comes on).
inline uint32_t ipv4Now() {
  static const long delay = [] {
    const char* v = std::getenv("CPK_TEST_IP_DELAY_MS");
    return v && *v ? std::atol(v) : 0L;
  }();
  const RadioState& r = radio();
  if (delay > 0 && r.on && nowMs() - r.onAtMs < delay) return 0;
  return ipv4();
}

// The radio was just turned on and the Kindle has not joined a network yet.
// It rejoins a known network within a few seconds; give it up to 20.
inline bool radioWarmingUp() {
  const RadioState& r = radio();
  return r.managed && r.on && !ipv4Now() && nowMs() - r.onAtMs < 20000;
}

}  // namespace kindle_net
