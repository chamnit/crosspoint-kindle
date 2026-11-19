// Power log: once a minute, one line in crosspoint.log with the battery
// reading, how busy CrossPoint and the whole system were, how often the main
// loop ran, the light level and the Wi-Fi radio state. Compare it with
// bin/powerlog.sh, which logs the same fields while the stock reader runs.
//
// CPK_POWER_LOG=0 turns it off; CPK_POWER_LOG_SEC sets the interval.

#include "KindlePowerLog.h"

#include <glob.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "KindleNet.h"

namespace {

long long nowMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

bool enabled() {
  static const bool on = [] {
    const char* v = std::getenv("CPK_POWER_LOG");
    return !(v && *v == '0');
  }();
  return on;
}

long long intervalMs() {
  static const long long ms = [] {
    const char* v = std::getenv("CPK_POWER_LOG_SEC");
    const long sec = v && *v ? std::atol(v) : 60;
    return static_cast<long long>(sec > 0 ? sec : 60) * 1000;
  }();
  return ms;
}

std::string firstGlob(const char* pattern) {
  std::string found;
  glob_t g{};
  if (glob(pattern, 0, nullptr, &g) == 0 && g.gl_pathc > 0) found = g.gl_pathv[0];
  globfree(&g);
  return found;
}

// First integer in a sysfs file ("85%", "3950 mV" and "-45" all work).
bool readInt(const std::string& path, long& out) {
  if (path.empty()) return false;
  FILE* f = std::fopen(path.c_str(), "r");
  if (!f) return false;
  const bool ok = std::fscanf(f, "%ld", &out) == 1;
  std::fclose(f);
  return ok;
}

struct Paths {
  std::string capacity, voltage, current, light;
};

const Paths& paths() {
  static const Paths p = [] {
    Paths r;
    const std::string dir = firstGlob("/sys/devices/system/*_battery/*_battery0");
    if (!dir.empty()) {
      r.capacity = dir + "/battery_capacity";
      r.voltage = dir + "/battery_voltage";
      r.current = dir + "/battery_current";
      // List what the battery driver offers, once, so the names can be checked.
      std::string names;
      glob_t g{};
      if (glob((dir + "/*").c_str(), 0, nullptr, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc; ++i) {
          const char* slash = std::strrchr(g.gl_pathv[i], '/');
          names += " ";
          names += slash ? slash + 1 : g.gl_pathv[i];
        }
      }
      globfree(&g);
      std::fprintf(stderr, "[power] battery driver %s:%s\n", dir.c_str(), names.c_str());
    } else {
      std::fprintf(stderr, "[power] no battery driver found\n");
    }
    r.light = firstGlob("/sys/class/backlight/*/brightness");
    return r;
  }();
  return p;
}

// CrossPoint's own CPU time (user + system) in clock ticks.
long long processTicks() {
  FILE* f = std::fopen("/proc/self/stat", "r");
  if (!f) return -1;
  char buf[1024];
  const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
  std::fclose(f);
  buf[n] = 0;
  const char* p = std::strrchr(buf, ')');  // the name may contain spaces
  if (!p) return -1;
  // After ")" come fields 3, 4, ...; utime and stime are fields 14 and 15.
  long long utime = 0, stime = 0;
  if (std::sscanf(p + 2, "%*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %lld %lld", &utime,
                  &stime) != 2)
    return -1;
  return utime + stime;
}

// Whole-system CPU ticks: total and busy (everything but idle and I/O wait).
bool systemTicks(long long& total, long long& busy) {
  FILE* f = std::fopen("/proc/stat", "r");
  if (!f) return false;
  long long v[8] = {};
  const int n = std::fscanf(f, "cpu %lld %lld %lld %lld %lld %lld %lld %lld", &v[0], &v[1], &v[2],
                            &v[3], &v[4], &v[5], &v[6], &v[7]);
  std::fclose(f);
  if (n < 4) return false;
  total = 0;
  for (int i = 0; i < n; ++i) total += v[i];
  busy = total - v[3] - (n > 4 ? v[4] : 0);
  return true;
}

struct Sample {
  long long ms = 0, procTicks = 0, sysTotal = 0, sysBusy = 0;
  unsigned long loops = 0;
};

unsigned long loopCount = 0;
Sample last;
long long startMs = 0;

std::string batteryText() {
  const Paths& p = paths();
  std::string out;
  char buf[48];
  long v = 0;
  if (readInt(p.capacity, v)) {
    std::snprintf(buf, sizeof(buf), "%ld%%", v);
    out += buf;
  }
  if (readInt(p.voltage, v)) {
    std::snprintf(buf, sizeof(buf), " %ld mV", v);
    out += buf;
  }
  if (readInt(p.current, v)) {
    std::snprintf(buf, sizeof(buf), " %ld mA", v);
    out += buf;
  }
  return out.empty() ? "?" : out;
}

void take(Sample& s) {
  s.ms = nowMs();
  s.procTicks = processTicks();
  systemTicks(s.sysTotal, s.sysBusy);
  s.loops = loopCount;
}

}  // namespace

void kindlePowerLogFrame() { ++loopCount; }

void kindlePowerLogTick() {
  if (!enabled()) return;
  const long long now = nowMs();
  if (startMs == 0) {
    startMs = now;
    take(last);
    paths();
    return;
  }
  if (now - last.ms < intervalMs()) return;
  Sample s;
  take(s);
  const double secs = (s.ms - last.ms) / 1000.0;
  const double hz = static_cast<double>(sysconf(_SC_CLK_TCK));
  const double procPct =
      s.procTicks >= 0 && last.procTicks >= 0 ? 100.0 * (s.procTicks - last.procTicks) / hz / secs
                                              : -1;
  const long long sysSpan = s.sysTotal - last.sysTotal;
  const double sysPct = sysSpan > 0 ? 100.0 * (s.sysBusy - last.sysBusy) / sysSpan : -1;
  const double loopsPerSec = (s.loops - last.loops) / secs;
  long light = -1;
  readInt(paths().light, light);
  std::fprintf(stderr,
               "[power] up %lld min | battery %s | CrossPoint CPU %.1f%% | system busy %.1f%% | "
               "loop %.0f/s | light %ld | wifi %s\n",
               (now - startMs) / 60000, batteryText().c_str(), procPct, sysPct, loopsPerSec,
               light, kindle_net::radioOn() ? (kindle_net::ipv4() ? "on" : "on, no address") : "off");
  last = s;
}

void kindlePowerLogNote(const char* what) {
  if (!enabled()) return;
  std::fprintf(stderr, "[power] %s | battery %s\n", what, batteryText().c_str());
}
