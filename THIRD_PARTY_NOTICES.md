# Licenses and third-party notices

## Short version

- **The source code in this repository** is MIT-licensed (see `LICENSE`).
- **The release binary** (`bin/crosspoint` in the release zip) statically links
  [FBInk](https://github.com/NiLuJe/FBInk), which is GPL-3.0-or-later. The
  binary as a whole is therefore distributed under the **GNU GPL, version 3**
  (`licenses/GPL-3.0.txt`). The upstream CrossPoint firmware ships its own
  binaries under GPLv3 for the same kind of reason (it links wolfSSL).
- Each component keeps its own license for its own code. The GPL applies to
  the combined binary. It does not change the MIT terms of this repository's
  source.

## Corresponding Source

GPLv3 requires that whoever distributes the binary also makes its complete
source available. For each release, that source is:

- **This repository** at the release tag (GitHub also offers it as a
  source archive on the release page).
- **The upstream projects at the exact revisions in the table below**, all
  public on GitHub. `tools/setup.sh` fetches exactly these revisions, and
  `tools/build.py` rebuilds the binary from them (see "Building" in
  `README.md`).

GPLv3 section 6(d) allows the source to be offered this way, from a network
server, including one run by a third party, as long as these directions stay
next to the binary and the source stays available. If an upstream copy ever
disappears, ask in this repository's issues and the maintainer will provide
that source.

## Components in the release binary

| Component | Revision | License | Source |
| --- | --- | --- | --- |
| This port (Kindle HAL, launcher, build tools) | release tag | MIT | this repository |
| [crosspoint-simulator](https://github.com/crosspoint-reader/crosspoint-simulator) (base of this repository) | `84c3421` | MIT, © Julia Nguyen | this repository |
| [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader) firmware and FreeInk SDK | [`6e7caa2`](https://github.com/crosspoint-reader/crosspoint-reader/tree/6e7caa222819fe20597439f885cb3c4f6bb78567) (SDK submodule `634a3f8`) | MIT, © CrossPoint Reader organization, plus the third-party code, fonts and data listed in its `licenses/` folder | upstream |
| [FBInk](https://github.com/NiLuJe/FBInk) (`fbink.c`, `cutef8`; built with `FBINK_MINIMAL`) | [`ded92fa`](https://github.com/NiLuJe/FBInk/tree/ded92fa510c957e0157c92ae83bd5c51af710c05) | GPL-3.0-or-later, © NiLuJe. `cutef8/utf8.c` is public domain (Jeff Bezanson); `cutef8/dfa.c` is MIT (Björn Höhrmann) | upstream |
| [OpenSSL](https://github.com/openssl/openssl) (libcrypto, libssl) | [`openssl-3.0.15`](https://github.com/openssl/openssl/tree/openssl-3.0.15) | Apache-2.0 | upstream |
| [ArduinoJson](https://github.com/bblanchon/ArduinoJson) | [`v7.4.2`](https://github.com/bblanchon/ArduinoJson/tree/v7.4.2) | MIT, © Benoit Blanchon | upstream |
| [QRCode](https://github.com/ricmoo/QRCode) | [`eafbde4`](https://github.com/ricmoo/QRCode/tree/eafbde494979abc2445c363cc2602230bcbe299c) | MIT, © Richard Moore and Project Nayuki | upstream |
| [AnimatedGIF](https://github.com/bitbank2/AnimatedGIF) | [`d01888f`](https://github.com/bitbank2/AnimatedGIF/tree/d01888f0255fd0781fc2b8600b9111b14c999584) | Apache-2.0, © BitBank Software | upstream |
| musl libc, LLVM libc++/libc++abi/libunwind/compiler-rt (supplied by the Zig 0.17.1 compiler) | Zig 0.17.1 | MIT (musl); Apache-2.0 WITH LLVM-exception (LLVM runtimes) | [ziglang.org](https://ziglang.org) |

The firmware's `licenses/README.md` lists its third-party material for the
ESP32 builds. Only part of that list is compiled into this Linux build. For
example, the ESP-IDF, Arduino-ESP32, NimBLE and wolfSSL code is replaced by
the simulator's host shims and OpenSSL. The fonts, hyphenation data,
icons and borrowed code that the firmware's UI uses are included, and their
notices apply.

## Where the license texts are

In the release zip, under `extensions/crosspoint/licenses/`:

| File | What |
| --- | --- |
| `LICENSE` | This repository (MIT) |
| `THIRD_PARTY_NOTICES.md` | This file |
| `GPL-3.0.txt` | GNU GPL v3 (FBInk; the combined binary) |
| `Apache-2.0.txt` | Apache License 2.0 (OpenSSL, AnimatedGIF) |
| `firmware/` | The firmware's `LICENSE` and its `licenses/` folder (fonts, data, borrowed code, other license texts) |
| `libraries/` | ArduinoJson, QRCode and AnimatedGIF license files |

In this repository: `LICENSE`, this file, and `licenses/` (GPL-3.0 and
Apache-2.0 texts).

## Trademarks

Kindle and Paperwhite are trademarks of Amazon.com, Inc. or its affiliates.
This project is not affiliated with or endorsed by Amazon, or by the
CrossPoint Reader project.
