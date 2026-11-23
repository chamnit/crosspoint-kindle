# CLAUDE.md

Guidance for AI coding agents (Claude Code and others; `AGENTS.md` links
here) working in this repository. Read `README.md` first: it covers usage,
the touch layout, settings and the build.

## What this repo is

A port of CrossPoint Reader (ESP32 e-reader firmware) to jailbroken Kindles:
first the Paperwhite 3 (7th gen, i.MX6SL, 1072x1448 portrait, 300 ppi, touch
only), now also the Scribe (1st gen) and, untested, the Paperwhite 2; one
binary for all (README "Supported models", `docs/models.md`). It is a fork of `crosspoint-simulator` at `84c3421` (the first
commit): the simulator's host HAL compiles the firmware as a Linux program;
this fork replaces SDL with the Kindle framebuffer (FBInk), evdev input and
sysfs, and cross-compiles one static ARM binary. Changes in the simulator's
own files are marked `Kindle:`; new files are `src/Kindle*`, `kual/`,
`scriptlets/`, `tools/`. Firmware revision: `6e7caa2`.

## Build and test

```sh
tools/setup.sh   # once; pins every dependency
tools/build.py --firmware ../crosspoint-reader --deps ../deps \
               --sysroot ../kindle-sysroot --tc ../kindle-tc/kindle- -j8
tools/package.sh # build/crosspoint-kindle-v<VERSION>.zip (+ fonts/ from ../fonts-src)
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
`KindleSettings.cpp`); a 32-bit framebuffer index (`DirectPixelWriter.h`); Screen Margin up to 250
(`CrossPointSettings.h`, for the Scribe's long lines). An
`old` string that no longer matches stops the build.

Off-device runs: `qemu-arm build/crosspoint` with `CPK_FAKE_FB=<dir>` (frames
as PGM; `CPK_FAKE_PANEL=1860x2480` for a Scribe-sized panel, default PW3), `CROSSPOINT_SIM_SD=<dir>` (storage root) and
`CROSSPOINT_SIM_INPUT_SCRIPT` (scripted taps/swipes in *logical* pixels,
853x1152 portrait on the PW3 at 125%, 930x1240 on the Scribe at its default 200%; times are absolute within a segment, each `WAIT`
starts a segment and matches a log-line regex). Scripted input bypasses
`KindleInput` (the unit test covers that) and keeps the stock idle timing
unless `CPK_TEST_IDLE_WAIT=1`. Other test hooks: `CPK_TEST_SLEEP=1` with
`CPK_POWER_STATE_PATH`/`CPK_HALL_PATH`, `CPK_TEST_POWER_TAP_MS`,
`CPK_TEST_IP_DELAY_MS`, `CPK_POWER_LOG_SEC`.

CI: `.github/workflows/build.yml` runs the steps above on every push (setup,
build, unit test, 20 s qemu start-up runs at PW3, PW2 and Scribe sizes, package); a `v*` tag matching
`VERSION` publishes the release with its CHANGELOG section.

## On-device testing

- Deploy: copy `build/pkg/extensions/crosspoint` to `/mnt/us/extensions/`
  (USB drive `extensions/`). Modern jailbreaks (hdnext, 5.16.3+) have no KUAL:
  also copy `build/pkg/documents/*.sh` to `/mnt/us/documents/`; these
  scriptlets (`scriptlets/`) find the app in `extensions/crosspoint` or a
  future KPM install (`/mnt/us/kmc/kpm/packages/crosspoint`). Their
  `# Icon: icons/x.png` line is replaced by the PNG as a data: URI when
  packaging (`scriptlets/icons/`, drawn by `tools/make_icons.js`).
- Log: `/mnt/us/crosspoint/crosspoint.log` (previous run: `.old`).
- With a shell (USBNetwork or Wi-Fi SSH): `sh
  /mnt/us/extensions/crosspoint/bin/crosspoint.sh`. The launcher pauses `cvm`
  with SIGSTOP and its EXIT trap must always resume it. If a run hangs:
  `killall crosspoint; kill -CONT $(pidof cvm)`.

## Invariants

- Logical sizes come from `kindle_geometry::get()` (panel from FBInk, zoom),
  never from `EInkDisplay::DISPLAY_*`, which are the largest supported panel
  (Scribe), used only for storage.
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

- Fonts: one built in (Noto Sans). The zip's `fonts/` carries six OFL TTF
  families from google/fonts (pinned in `tools/setup.sh`, listed in
  `tools/package.sh`, each with its `OFL.txt`); the firmware loads TTF/OTF
  from `/mnt/us/fonts` because `BOARD_HAS_PSRAM` enables vector fonts.

- Source is MIT. The binary links FBInk (GPL-3.0-or-later), so it is GPLv3 as
  a whole. Its source offer is this repo at the tag plus the pinned upstream
  revisions in `THIRD_PARTY_NOTICES.md`; keep that table in step with
  `tools/setup.sh`. `tools/package.sh` puts all license texts in the zip.
- Release steps are in the README ("Making a release"); version in `VERSION`,
  history in `CHANGELOG.md`. Device reports come through
  `.github/ISSUE_TEMPLATE/device-report.yml` (probe.txt + crosspoint.log).
  After each report or on-device test, update the README "Test status" table
  (feature x model tally) and `docs/models.md`.

## Other Kindles (Scribe)

`docs/models.md` has the facts per model and what changed for the Scribe
(1st gen, 1860x2480, MediaTek, firmware 5.19.4: works in first device tests). The panel size comes from FBInk at startup
(`kindle_screen::panelSize`); `EInkDisplay::DISPLAY_*` are the largest
supported panel (Scribe), for storage only. The default zoom scales with
the panel width (125% on the PW3, 200% cap on the Scribe). Off-device: `CPK_FAKE_PANEL=WxH`
(portrait) with `CPK_FAKE_FB`. Input devices are chosen by capability
(`classifyDevice`); pen digitizers are grabbed so the Kindle's ink handler
cannot draw over CrossPoint. New models: run the probe (`crosspoint --probe`,
KUAL "Probe this Kindle", scriptlet "CrossPoint Probe" -> `probe.txt`) and add
paths to the search lists rather than per-model code where possible. FBInk
handles the MediaTek display controller, which is why it stays.
