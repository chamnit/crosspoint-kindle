# Kindle models

Facts per model for the port, from `crosspoint --probe` / `bin/probe.sh`
(KUAL "Probe this Kindle", scriptlet "CrossPoint Probe").

| | Paperwhite 3 (7th gen) | Scribe (1st gen) |
| --- | --- | --- |
| Status | Supported, tested | Probed only |
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
| Kindle UI process | `cvm` (paused by the launcher) | no `cvm`; to be found (probe v2 lists processes) |
| Waveforms | GL16, GC16 | MTK; FBInk reports eclipse (night) waveforms, `can_wait_for_submission` |

## Scribe: what the current build does and must change

- The static binary runs as is (`--probe` exited 0).
- `KindleScreen::begin` rejects any panel but 1072 x 1448: the panel size
  must come from FBInk at run time (and the compile-time maximums in
  `EInkDisplay.h`, `BoardConfig.h` and `HalDisplay.cpp` must cover 1860 x 2480).
- `openTouchDevice()` takes the first device with ABS_X/ABS_Y: on the Scribe
  that is the accelerometer (event1). It must prefer multitouch devices.
- The battery path and the cover path above are not in the search lists yet.
- The launcher pauses `cvm`, which does not exist here; it must pause whatever
  draws the UI and reads touch on this firmware.
