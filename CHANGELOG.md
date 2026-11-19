# Changelog

## Unreleased

- KUAL "Probe this Kindle (for new models)": a read-only report of the
  device (FBInk's identification, framebuffer, input devices, battery, light,
  cover, sleep) in `crosspoint/probe.txt`, backed by `crosspoint --probe`.
  First step towards other models (Kindle Scribe).
- GitHub Actions build: every push is built and tested (unit test and a
  start-up run under qemu); pushing a `v*` tag publishes the release.
- Scriptlets for jailbreaks without KUAL (firmware 5.16.3+): the zip's
  `documents/` folder has "CrossPoint Reader" and "CrossPoint Probe", which
  appear in the Library and start the app or the probe.
- The zip now carries `VERSION`.

## v0.1.0 (2026-11-19)

First public release. CrossPoint Reader (firmware `6e7caa2`) as a KUAL app on
the jailbroken Kindle Paperwhite 3 (7th gen). Tested on one PW3.

### Platform
- Port of the CrossPoint desktop simulator (`84c3421`) to the Kindle: SDL
  replaced by the e-ink framebuffer (FBInk), the touchscreen, power button
  and cover sensor. One static ARM binary (Zig clang, musl, Cortex-A9).
- Build without PlatformIO (`tools/build.py`); every firmware edit is listed
  in `SOURCE_PATCHES` with its reason. One-shot setup with every dependency
  pinned (`tools/setup.sh`).
- KUAL launcher that pauses and fully restores the Kindle UI (status bar and
  clock, screensaver, pillow, firewall, Wi-Fi).
- Exit from Home → "Exit CrossPoint…" (with confirmation).

### Screen
- Changed-rectangle refreshes through FBInk; each page is one 16-gray refresh.
- Zoom (`CPK_ZOOM`, default 125%) so the UI is a sensible size at 300 ppi.
- Fixed 16-bit buffer offsets that garbled the logo and book images on the
  larger panel.

### Touch
- Direct evdev touch input, grabbed from the paused Kindle UI.
- Taps and swipes both work, as on stock touch CrossPoint. Page taps always
  work, whatever the page-turn gesture setting says (its firmware default is
  "swipe only").
- Zones: light (top centre), reader menu (bottom centre, or swipe up from the
  bottom edge), Back (left edge in menus), previous/next page (left third /
  rest). Two-finger tap = Home.

### Power
- Sleep with the power button or the cover; wake the same way. Kernel suspend
  done directly (powerd stays Active while the Kindle UI is paused), with a
  soft-sleep fallback.
- Quick power-button taps (press and release in one input poll) now count.
- Power presses ignored for 3 s after waking.
- Wi-Fi off while CrossPoint runs; on only for File Transfer.
- Idle main loop blocks on input instead of polling: about 2 wake-ups a second
  instead of about 100 when idle.
- Once-a-minute power log, and a matching stock-reader log from KUAL.
- Logging of every power-button event and of what woke the Kindle.

### Books and transfer
- Battery level from the Kindle's gas gauge; front light via powerd.
- Wi-Fi file transfer over the Kindle's own connection (ports 80/81).
- Library and Browse Files skip the Kindle system folders and the
  jailbreak/KOReader folders.

### Removed during development
- The two-finger 3 s hold to exit: it gave no feedback and was easy to
  trigger by accident. Home → "Exit CrossPoint…" is the way out.
- Touch calibration (the PW3 touchscreen already reports panel coordinates;
  `CPK_TOUCH_SWAP_XY`/`MIRROR_*` remain for odd panels).
