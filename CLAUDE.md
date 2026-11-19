# CLAUDE.md

Guidance for AI coding agents (Claude Code and others; `AGENTS.md` links
here) working in this repository. Read `README.md` first: it covers usage,
the touch layout, settings and the build.

## What this repo is

A port of CrossPoint Reader (ESP32 e-reader firmware) to the jailbroken
Kindle Paperwhite 3 (7th gen, i.MX6SL, 1072x1448 portrait, 300 ppi, touch
only). It is a fork of `crosspoint-simulator` at `84c3421` (the first
commit): the simulator's host HAL compiles the firmware as a Linux program;
this fork replaces SDL with the Kindle framebuffer (FBInk), evdev input and
sysfs, and cross-compiles one static ARM binary. Changes in the simulator's
own files are marked `Kindle:`; new files are `src/Kindle*`, `kual/`,
`tools/`. Firmware revision: `6e7caa2`.

## Build and test

```sh
tools/setup.sh   # once; pins every dependency
tools/build.py --firmware ../crosspoint-reader --deps ../deps \
               --sysroot ../kindle-sysroot --tc ../kindle-tc/kindle- -j8
tools/package.sh # build/crosspoint-kindle-pw3-v<VERSION>.zip
g++ -std=c++17 -Isrc tests/kindle_input_test.cpp -o /tmp/kit && /tmp/kit
```

No PlatformIO: `tools/build.py` reproduces the simulator's source set and
flags. It compiles from `build/fw`, a hard-link mirror of the firmware in
which only `SOURCE_PATCHES` files are edited copies (headers included); the
firmware checkout is never modified. Each patch carries its reason. They
cover: board tag; Home menu "Exit CrossPoint..." (with confirmation); one-time
Kindle steps after settings load and the idle wait (`main.cpp`); reader touch
zones and always-on page taps (`ReaderUtils.h`); light tap and screen tracking
(`ActivityManager.cpp`); hidden Kindle/jailbreak folders (`LibraryBuilder.cpp`,
`FileBrowserActivity.cpp`, kept in step, plus the `.kindle-library-N` marker in
`KindleSettings.cpp`); a 32-bit framebuffer index (`DirectPixelWriter.h`). An
`old` string that no longer matches stops the build.

Off-device runs: `qemu-arm build/crosspoint` with `CPK_FAKE_FB=<dir>` (frames
as PGM), `CROSSPOINT_SIM_SD=<dir>` (storage root) and
`CROSSPOINT_SIM_INPUT_SCRIPT` (scripted taps/swipes in *logical* pixels,
853x1152 portrait at 125%; times are absolute within a segment, each `WAIT`
starts a segment and matches a log-line regex). Scripted input bypasses
`KindleInput` (the unit test covers that) and keeps the stock idle timing
unless `CPK_TEST_IDLE_WAIT=1`. Other test hooks: `CPK_TEST_SLEEP=1` with
`CPK_POWER_STATE_PATH`/`CPK_HALL_PATH`, `CPK_TEST_POWER_TAP_MS`,
`CPK_TEST_IP_DELAY_MS`, `CPK_POWER_LOG_SEC`.

## On-device testing

- Deploy: copy `build/pkg/extensions/crosspoint` to `/mnt/us/extensions/`
  (USB drive `extensions/`).
- Log: `/mnt/us/crosspoint/crosspoint.log` (previous run: `.old`).
- With a shell (USBNetwork or Wi-Fi SSH): `sh
  /mnt/us/extensions/crosspoint/bin/crosspoint.sh`. The launcher pauses `cvm`
  with SIGSTOP and its EXIT trap must always resume it. If a run hangs:
  `killall crosspoint; kill -CONT $(pidof cvm)`.

## Invariants

- Logical sizes come from `kindle_geometry::get()` (zoom), never from
  `EInkDisplay::DISPLAY_*`, which are the panel maximums used for storage.
- No 16-bit framebuffer offsets: they fit the 800x480 Xteink (48 KB) but wrap
  on this panel. Two were found (`HalDisplay::drawImage`, `DirectPixelWriter`).
- The buffer-to-panel mapping is fixed (`HalDisplay.cpp panelMapping`, always
  RotateCW); the firmware rotates content itself. `HalGPIO.cpp
  panelToLogical()` must stay its exact inverse for each orientation.
- Pages are presented once: the B/W base only records the requested refresh
  mode and the gray pass presents base plus grays together.
  `cleanupGrayscaleBuffers()` must still present a pending base if the gray
  pass never happens. `KindleScreen` refreshes only the changed rectangle;
  FULL_REFRESH sends the whole panel with a flash.
- Startup failures exit (3 = no display, 4 = no touch) so the launcher
  restores the Kindle UI. Never leave the user on a frozen screen.
- HAL stub rule (from the simulator): when newer firmware calls a new HAL
  method, add a matching no-op in `src/Hal*.h`, mirroring its signature.
- `simulator_main.cpp` ends with `_exit()`: the render task never returns, so
  global destructors must not run.

## Input

- Touch layout is in the README table. `HalGPIO::wasSwipe` passes swipes in
  every direction. Menus' left 10% = Back (synthesized in `kindleTouchUp`,
  not in the reader or keyboards). Two-finger tap = Home (`KindleInput.cpp`);
  longer two-finger holds do nothing. The only exit is Home > "Exit
  CrossPoint..." (patched `HomeActivity.cpp`, with confirmation).
- Page taps always work (`gestureAllowsTap` patched): the firmware default
  for both page-turn gestures is SWIPE_ONLY, and a reset settings file once
  left tap page turns off. Swipes follow the setting.
- A press and release read in the same poll (power key, cover) are split
  across two frames (`powerReleasePending`, `coverPressPending`), otherwise
  the firmware sees a 0 ms hold and ignores it.
- Idle: the patched main loop calls `kindleIdleWait()`, which blocks in
  `poll()` on the touch and power-key fds (500 ms cap) unless input is in
  flight.

## Sleep, power, Wi-Fi

- Sleep/wake (`kindleSleepUntilWake` in HalGPIO.cpp): powerd stays Active
  while cvm is paused, so CrossPoint writes "mem" to /sys/power/state itself;
  soft sleep if refused. Queued power events are dropped before sleeping;
  power presses are ignored for 3 s after the wake restart (CPK_JUST_WOKE).
  The cover is the hall sensor sysfs file (`wario_hall0/hall_detect` on PW3,
  1 = closed), polled 2x/s. The log records every power-key event and the
  interrupts that changed across each suspend.
- Wi-Fi radio (`KindleNet.h setRadio`) is managed only when the launcher
  exports `CPK_WIFI_WAS_TURNED_OFF=1`. `WiFi.mode()` and `ESP.restart()` (a
  no-op restart on this port) drive it; `disconnect(wifioff)` does not (the
  firmware uses it to cancel joins).
- Power log: `KindlePowerLog.cpp`; stock-reader baseline:
  `kual/crosspoint/bin/powerlog.sh`.

## Releases and licensing

- Source is MIT. The binary links FBInk (GPL-3.0-or-later), so it is GPLv3 as
  a whole. Its source offer is this repo at the tag plus the pinned upstream
  revisions in `THIRD_PARTY_NOTICES.md`; keep that table in step with
  `tools/setup.sh`. `tools/package.sh` puts all license texts in the zip.
- Release steps are in the README ("Making a release"); version in `VERSION`,
  history in `CHANGELOG.md`.

## Next: other Kindles (e.g. Scribe)

Not done yet. The PW3 panel size is a compile-time constant in a few places
(`EInkDisplay.h`, `BoardConfig.h`, `HalDisplay.cpp pixelBuf`); other models
need it at run time from the framebuffer, plus per-model battery/light/cover/
input paths. FBInk already handles the MediaTek display controller used by
newer Kindles (Scribe, PW5), which is why it stays.
