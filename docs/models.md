# Kindle models

Facts per model for the port, from `crosspoint --probe` / `bin/probe.sh`
(KUAL "Probe this Kindle", scriptlet "CrossPoint Probe").

| | Paperwhite 3 (7th gen) | Scribe (1st gen) |
| --- | --- | --- |
| Status | Supported, tested | Works in first tests: start, menus, file browser, Back, reading with images, exit |
| Firmware tested | 5.x, KUAL jailbreak | 5.19.4, hdnext jailbreak (KPM, scriptlets, no KUAL) |
| SoC / userland | i.MX6SL, Cortex-A9 / armel | MediaTek, Cortex-A53 (`0xd03`) in 32-bit / armhf |
| RAM | 512 MB | 1 GB (about 550 MB available with the UI running) |
| FBInk name | PW3 | Scribe, codename Barolo, platform Bellatrix3, id `0x874` |
| Panel (portrait) | 1072 x 1448, 300 ppi | 1860 x 2480, 300 ppi |
| Framebuffer | 8 bpp | `hwtcon_v2`, 8 bpp, stride 1872 (rows padded), rota 3, virtual 1872 x 4960 |
| Touch | multitouch | `pt_mt`, 2 slots, x 0..1859, y 0..2479 (panel pixels, no swap or mirror), `ABS_MT_TOOL_TYPE`, no `BTN_TOUCH` |
| Pen | none | `WacomDigitizer` (raw, x 0..15624, y 0..20832) and `stylus-custom` (panel pixels); pressure 0..4095, tilt, keys 320 321 330 331 332 |
| Other ABS devices | none | `kx132-accel` (ABS_X/ABS_Y): must not be taken for touch |
| Power key | KEY_POWER | `bd71828-pwrkey`, KEY_POWER (116) |
| Battery | `wario_battery0/battery_capacity` | `/sys/class/power_supply/bd71827_bat/capacity` |
| Front light | powerd `flIntensity` 0..24 | powerd `flIntensity` 0..24 (sysfs `fp9966-bl0`, `fp9966-bl1`, 0..2047) |
| Cover | `wario_hall0/hall_detect` | `/sys/devices/platform/eink_hall/hall_detect` (value meaning not yet checked) |
| Sleep | `/sys/power/state` mem | `/sys/power/state`: `freeze mem` |
| Kindle UI | `cvm` (paused by the launcher) | no `cvm`: `KPPMainApp` draws through `Xorg`; `awesome` (window manager, paused like KOReader does on 5.19.4), `pillowd`, `JunoStatusBarDriver` (statusbar job, stopped). `Xorg` reads touch, pen and accelerometer; `WetInkHandler` reads the raw pen and draws ink into fb0 |
| Waveforms | GL16, GC16 | MTK; FBInk reports eclipse (night) waveforms, `can_wait_for_submission` |

## Paperwhite 2 (6th gen, Pinot): first report

From a user's issue with v0.1.0 (stock firmware, kernel 3.0.35-lab126,
KUAL): the launcher, FBInk detection and cleanup all work; v0.1.0 then
stopped because the panel (758 x 1024, 212 ppi) was not the PW3's fixed
size. The run-time panel size made for the Scribe removes that check. Same
SoC, kernel and PMIC (MAX77696) as the PW3; the reporter gives
`/sys/class/backlight/max77696-bl/` and
`/sys/class/power_supply/max77696-battery/` (now in the battery search).
Default zoom works out to 100% (212 ppi is close to CrossPoint's design).
Not yet known: touch device, cover sensor, memory headroom (256 MB RAM).

## Scribe: changes made for it

- Panel size from FBInk at startup (`KindleScreen` `panelSize`); the
  compile-time maximums (`EInkDisplay::DISPLAY_*`) are now the Scribe's
  2480 x 1860. The default zoom scales with the panel width: 200% (the cap)
  on the Scribe, so the firmware draws 1240 x 930, close to the PW3's
  1152 x 853 layout. (At 125% it drew 1984 x 1488: text and tap targets too
  small on the first device test.)
- Touch device choice prefers multitouch, and skips the accelerometer and pen
  digitizers (`classifyDevice`, unit-tested with the Scribe's devices).
- Pen devices are grabbed while CrossPoint runs, so `WetInkHandler` cannot draw
  ink over it. The pen is not an input yet.
- Battery `bd71827_bat`, cover `/sys/devices/platform/*hall*/hall_detect`.
- Launcher: no `cvm` is normal on this firmware; `awesome`, pillow and the
  statusbar job are handled as on the PW3.

Image sizes follow the firmware's rules (natural pixel size unless the book's
CSS says otherwise), so on the larger page low-resolution images a book
stretches to full width look large and others look small; this is expected.

Still unknown until tested on the device: waveforms and refresh speed on the
MediaTek controller, cover value when closed (assumed 1), suspend and wake,
the light, Wi-Fi, palm rejection (`ABS_MT_TOOL_TYPE` is not read).
