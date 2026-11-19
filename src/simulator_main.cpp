// Entry point for the Kindle build of CrossPoint.
//
// Same frame loop as the desktop simulator, without SDL. The launcher script
// (kual/crosspoint/bin/crosspoint.sh) pauses the Kindle UI before starting us
// and resumes it after we exit.

#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "Arduino.h"
#include "HalDisplay.h"
#include "HalGPIO.h"
#include "KindleInput.h"
#include "KindleScreen.h"
#include "SimulatorLifecycle.h"

extern void setup();
extern void loop();
extern HalDisplay display;  // defined in main.cpp
extern std::atomic<bool> quitRequested;

namespace {
volatile sig_atomic_t redrawRequested = 0;

void onQuitSignal(int) { quitRequested.store(true); }
// Sent by the launcher when powerd reports the Kindle left its screensaver
// (the stock UI may have drawn over the panel): repaint the last frame.
void onRedrawSignal(int) { redrawRequested = 1; }
}  // namespace

int main(int /*argc*/, char **argv) {
  setvbuf(stderr, nullptr, _IOLBF, 0);
  signal(SIGTERM, onQuitSignal);
  signal(SIGINT, onQuitSignal);
  signal(SIGHUP, onQuitSignal);
  signal(SIGUSR1, onRedrawSignal);
  signal(SIGPIPE, SIG_IGN);

  SimulatorLifecycle::initProcessArgs(argv);
  setup();
  while (!display.shouldQuit()) {
    // Clear input edge latches once per frame (see HalGPIO::update()).
    gpio.beginFrame();
    loop();
    if (redrawRequested) {
      redrawRequested = 0;
      kindle_screen::redrawLast();
    }
    // Short pause between frames, like the desktop simulator. Input is
    // polled non-blocking from loop(); when idle, the patched main loop
    // blocks on the input devices instead (kindleIdleWait in HalGPIO.cpp).
    usleep(1000);
  }
  kindle_input::end();
  std::fprintf(stderr, "[kindle] bye\n");
  // Skip global destructors: the render task never returns (see the
  // simulator's notes on _exit).
  _exit(SimulatorLifecycle::exitCode());
}
