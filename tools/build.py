#!/usr/bin/env python3
"""Build CrossPoint Reader for the Kindle Paperwhite 3 without PlatformIO.

The firmware normally builds the desktop simulator with PlatformIO's "native"
platform. This script compiles the same source set (firmware src/, lib/, the
FreeInk SDK libraries the simulator uses, and this repo's host HAL) with an ARM
cross compiler and links one static binary.

Usage:
  tools/build.py --firmware ../crosspoint-reader --deps ../deps \\
                 --sysroot ~/kindle-sysroot --tc ~/kindle-tc/kindle- [-j N]

  --deps     folder holding ArduinoJson (v7.4.2), AnimatedGIF and QRCode checkouts
  --sysroot  include/{fbink.h,openssl/} and lib/{libfbink.a,libssl.a,libcrypto.a}
  --tc       prefix of the compiler wrappers: <tc>cc, <tc>c++, <tc>ar
"""

import argparse
import concurrent.futures as futures
import os
import shlex
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent  # crosspoint-kindle

DEFINES = [
    "SIMULATOR",
    "CROSSPOINT_KINDLE_PW3",
    "SIMULATOR_DEVICE_METALIO_EINK4_PLUS",
    "FREEINK_DEVICE_METALIO_EINK4_PLUS=1",
    "BOARD_HAS_PSRAM",
    "CROSSPOINT_SIMULATOR_PROJECT_WEBSERVER",
    "ENABLE_SERIAL_LOG",
    "LOG_LEVEL=2",
    "EINK_DISPLAY_SINGLE_BUFFER_MODE=1",
    "MINIZ_NO_ZLIB_COMPATIBLE_NAMES=1",
    "XML_GE=0",
    "XML_CONTEXT_BYTES=1024",
    "USE_UTF8_LONG_NAMES=1",
    "PNG_MAX_BUFFERED_PIXELS=16416",
    "DISABLE_FS_H_WARNING=1",
    "DESTRUCTOR_CLOSES_FILE=1",
    "ARDUINOJSON_ENABLE_ARDUINO_STRING=1",
    "PLATFORMIO=60118",
]

# Firmware files the simulator excludes (firmware update paths).
SRC_EXCLUDE = {
    "network/FirmwareFlasher.cpp",
    "network/OtaBootSwitch.cpp",
    "network/OtaUpdater.cpp",
    "platform/skip_efuse_blk_check.c",
    "platform/S31NimbleMemory.cpp",
}

# Every change this port makes to the firmware itself, as exact text edits:
# file -> [(old, new)]. The build compiles from build/fw, a hard-link mirror of
# the firmware checkout in which only these files are real (edited) copies, so
# the checkout itself is never modified. If the firmware changes and an `old`
# string no longer matches, the build stops and names the file (pinned
# firmware revision: see tools/setup.sh). Injected code is marked "Kindle".
SOURCE_PATCHES = {
    # Board name reported to the web UI and logs.
    "src/network/FirmwareBoardTag.cpp": [
        ("#if FREEINK_DEVICE_X4PRO\n",
         '#if CROSSPOINT_KINDLE_PW3\n#define CROSSPOINT_BOARD_NAME "kindle_pw3"\n'
         "#elif FREEINK_DEVICE_X4PRO\n"),
    ],
    # Home menu: add "Exit CrossPoint..." as the last entry, behind a
    # confirmation (it sits under Settings and is easy to hit). Firmware on a
    # real e-reader never exits; an app on a Kindle has to.
    "src/activities/home/HomeActivity.cpp": [
        ('#include "fontIds.h"\n',
         '#include "fontIds.h"\n#include "activities/util/ConfirmationActivity.h"\n'
         '\nvoid kindleRequestExit();  // Kindle HAL (HalGPIO.cpp)\n'),
        ("  int count = 4;  // File Browser, Library, File transfer, Settings\n",
         "  int count = 5;  // File Browser, Library, File transfer, Settings, Exit (Kindle)\n"),
        ("    const int menuIndex = selectorIndex - static_cast<int>(recentBooks.size());\n",
         "    if (static_cast<int>(selectorIndex) == getMenuItemCount() - 1) {\n"
         "      // Ask first: the row sits under Settings and is easy to hit.\n"
         "      auto confirm = makeUniqueNoThrow<ConfirmationActivity>(\n"
         '          renderer, mappedInput, "Exit CrossPoint?",\n'
         '          "Go back to the Kindle home screen. Your reading position is saved.",\n'
         '          "Cancel", "Exit");\n'
         "      if (!confirm) return;\n"
         "      startActivityForResult(std::move(confirm), [](const ActivityResult& result) {\n"
         "        if (!result.isCancelled) kindleRequestExit();\n"
         "      });\n"
         "      return;\n"
         "    }\n"
         "    const int menuIndex = selectorIndex - static_cast<int>(recentBooks.size());\n"),
        ("  std::vector<UIIcon> menuIcons = {Folder, Library, Transfer, Settings};\n",
         "  std::vector<UIIcon> menuIcons = {Folder, Library, Transfer, Settings};\n"
         '  menuItems.push_back("Exit CrossPoint...");\n'
         "  menuIcons.push_back(None);\n"),
    ],
    # Kindle one-time steps (tap page-turn defaults, Library re-index; see
    # src/KindleSettings.cpp) right after the saved settings load.
    "src/main.cpp": [
        ("  SETTINGS.loadFromFile();\n  // Push the saved timezone",
         "  SETTINGS.loadFromFile();\n  kindleApplySettingsDefaultsOnce();\n  // Push the saved timezone"),
        ("\nvoid enterDeepSleep(bool fromTimeout = false) {",
         "\nvoid kindleApplySettingsDefaultsOnce();  // Kindle HAL (KindleSettings.cpp)\n"
         "void kindleIdleWait();  // Kindle HAL (HalGPIO.cpp)\n"
         "void enterDeepSleep(bool fromTimeout = false) {"),
        # Idle (3 s without input): instead of waking every 10 ms to look for
        # a button, block until the touchscreen or power key has an event, or
        # 500 ms pass (cover sensor, clock, timers). Saves CPU wakeups.
        ("      const unsigned long idleStart = millis();\n"
         "      while (millis() - idleStart < 50) {\n"
         "        delay(10);\n"
         "        if (gpio.rawInputActive()) break;\n"
         "      }\n",
         "      kindleIdleWait();\n"),
    ],
    # Kindle reader touch layout. Swipes work as in stock CrossPoint (sideways
    # = pages, bottom-edge up = menu). Taps; strips are 1/12 of the height:
    #   top strip:    centre third = light (ActivityManager), corners = pages
    #   bottom strip: centre third = reader menu, corners = pages
    #   (Home is the two-finger tap, handled in the HAL)
    #   elsewhere:    left third = previous page, rest = next page
    "src/activities/reader/ReaderUtils.h": [
        # Kindle: taps always turn pages, alongside swipes. "Swipe only" is
        # the firmware default, so a reset settings file would otherwise
        # silently drop tap page turns.
        ("inline bool gestureAllowsTap(const uint8_t gesture) {\n"
         "  return gesture == CrossPointSettings::TAP_AND_SWIPE || gesture == CrossPointSettings::TAP_ONLY ||\n"
         "         gesture == CrossPointSettings::INVERTED_TAP;\n"
         "}\n",
         "inline bool gestureAllowsTap(const uint8_t gesture) {\n"
         "  (void)gesture;  // Kindle: always (see tools/build.py)\n"
         "  return true;\n"
         "}\n"),
        # Swipe up from the bottom edge also opens the menu (the menu lives at
        # the bottom), alongside the bottom-centre tap.
        ("  if (SETTINGS.showReaderMenu == CrossPointSettings::READER_MENU_SWIPE_UP && input.wasReaderMenuSwipeUp()) {\n",
         "  if (input.wasReaderMenuSwipeUp()) {  // Kindle: whatever showReaderMenu says\n"),
        ("  const int zoneWidth = width / 3;\n  const int zoneHeight = height / 3;\n"
         "  return x >= zoneWidth && x < width - zoneWidth && y >= zoneHeight && y < height - zoneHeight;\n",
         "  // Kindle: the bottom-centre third of the bottom strip opens the menu.\n"
         "  return y >= height - height / 12 && x >= width / 3 && x < width - width / 3;\n"),
        ("  const int width = renderer.getScreenWidth();\n  const int height = renderer.getScreenHeight();\n"
         "  // Reserve the centered reader-menu tap target",
         "  const int width = renderer.getScreenWidth();\n  const int height = renderer.getScreenHeight();\n"
         "  // Kindle: bottom-centre = menu (isTouchMenuTap), top-centre = light\n"
         "  // (ActivityManager). The strip corners turn pages like the page below.\n"
         "  const bool kTop = y < height / 12, kBottom = y >= height - height / 12;\n"
         "  const bool kCentre = x >= width / 3 && x < width - width / 3;\n"

         "  if ((kBottom || kTop) && kCentre) return result;\n"
         "  // Reserve the centered reader-menu tap target"),
        ("  if (reserveReaderMenuTap && SETTINGS.showReaderMenu == CrossPointSettings::READER_MENU_TAP && x >= width / 3 &&\n"
         "      x < width - width / 3 && y >= height / 3 && y < height - height / 3) {\n",
         "  // Kindle: no centre menu zone (the menu is the bottom strip).\n"
         "  if (false && reserveReaderMenuTap) {\n"),
    ],
    # Light: a tap on the top-centre third of the top strip, on any screen
    # (stock CrossPoint: a tap on the status bar of a few screens). The corners
    # stay free for the header back arrow in menus and turn pages in the
    # reader. Also tells the HAL which screen is showing (the left-edge Back
    # strip is for menus only, not the reader or keyboard).
    "src/activities/ActivityManager.cpp": [
        ('#include "ActivityManager.h"\n',
         '#include "ActivityManager.h"\n\nvoid kindleSetActivity(const char* name, bool reader);  // Kindle HAL\n'),
        ("  if (currentActivity) {\n    if (!currentActivity->isHomeActivity() && mappedInput.wasHomeGesture()) {",
         "  if (currentActivity) {\n    kindleSetActivity(currentActivity->name.c_str(), isReaderActivity());\n"
         "    if (!currentActivity->isHomeActivity() && mappedInput.wasHomeGesture()) {"),
        ("    if (mappedInput.hasTouch() && (currentActivity->name == \"Home\" || currentActivity->name == \"FileBrowser\" ||\n"
         "                                   currentActivity->name == \"Settings\" || currentActivity->name == \"SettingsList\" ||\n"
         "                                   currentActivity->name == \"NetworkModeSelection\")) {",
         "    if (mappedInput.hasTouch()) {"),
        ("      statusBarTap = mappedInput.wasScreenTapped(tx, ty) && ty < 44 && !HeaderBackTapTarget::contains(tx, ty);",
         "      statusBarTap = mappedInput.wasScreenTapped(tx, ty) && ty < renderer.getScreenHeight() / 12 &&\n"
         "                     !HeaderBackTapTarget::contains(tx, ty) &&\n"
         "                     tx >= renderer.getScreenWidth() / 3 &&\n"
         "                     tx < renderer.getScreenWidth() - renderer.getScreenWidth() / 3;"),
    ],
    # Kindle system and jailbreak folders at the top of the drive: left out of
    # the Library and hidden in Browse Files. Keep both lists the same, and
    # bump the .kindle-library-N marker in src/KindleSettings.cpp when they
    # change. (Dot files and folders are already skipped by the Library;
    # Browse Files hides them unless "show hidden files" is on.)
    "lib/LibraryIndex/LibraryBuilder.cpp": [
        ('  constexpr std::string_view ROOTS[] = {"fonts", "dictionaries", "plugins", "sleep", "sleep-overlay"};\n',
         '  constexpr std::string_view ROOTS[] = {"fonts", "dictionaries", "plugins", "sleep", "sleep-overlay",\n'
         '                                       // Kindle system and jailbreak folders\n'
         '                                       "audible", "screenshots", "system", "voice", "extensions",\n'
         '                                       "koreader", "apps", "mrpackages", "mesquito", "libkh"};\n'),
    ],
    "src/activities/home/FileBrowserActivity.cpp": [
        ("    if ((!SETTINGS.showHiddenFiles && fileNameBuffer[0] == '.') ||\n"
         "        strcmp(fileNameBuffer.get(), \"System Volume Information\") == 0) {\n",
         "    if ((!SETTINGS.showHiddenFiles && fileNameBuffer[0] == '.') ||\n"
         "        strcmp(fileNameBuffer.get(), \"System Volume Information\") == 0 ||\n"
         "        (isDirectory && basepath == \"/\" && kindleIsSystemFolder(fileNameBuffer.get()))) {\n"),
        ("void FileBrowserActivity::loadFiles() {\n",
         "// Kindle system folders at the top of the drive, hidden from Browse Files.\n"
         "static bool kindleIsSystemFolder(const char* name) {\n"
         "  for (const char* folder : {\"audible\", \"fonts\", \"screenshots\", \"system\", \"voice\",\n"
         "                             \"extensions\", \"koreader\", \"apps\", \"mrpackages\", \"mesquito\", \"libkh\"}) {\n"
         "    if (strcasecmp(name, folder) == 0) return true;\n"
         "  }\n"
         "  return false;\n"
         "}\n\n"
         "void FileBrowserActivity::loadFiles() {\n"),
    ],
    # The framebuffer byte index was 16-bit: fine for 800x480 (48 KB), but it
    # wraps on the Kindle's larger buffer, scattering book images.
    "lib/Epub/Epub/converters/DirectPixelWriter.h": [
        ("const uint16_t byteIndex = static_cast<uint16_t>(sy * displayWidthBytes + (phyX >> 3));",
         "const uint32_t byteIndex = static_cast<uint32_t>(sy * displayWidthBytes + (phyX >> 3));"),
    ],
}

MIRROR_SKIP = {".git", "test", ".pio", "docs"}


def mirror_firmware(fw, dst):
    """Hard-link the firmware tree into dst; SOURCE_PATCHES files become edited copies."""
    import shutil
    patched = {}
    for rel, edits in SOURCE_PATCHES.items():
        text = (fw / rel).read_text()
        for old, new in edits:
            if old not in text:
                sys.exit("patch anchor missing in %s: %r" % (rel, old))
            text = text.replace(old, new, 1)
        patched[rel] = text
    for dirpath, dirnames, filenames in os.walk(fw):
        dirnames[:] = [d for d in dirnames if d not in MIRROR_SKIP]
        rel_dir = Path(dirpath).relative_to(fw)
        (dst / rel_dir).mkdir(parents=True, exist_ok=True)
        for f in filenames:
            rel = (rel_dir / f).as_posix()
            src, out = Path(dirpath) / f, dst / rel_dir / f
            same = out.exists() and out.stat().st_ino == src.stat().st_ino
            if rel in patched:
                if same:
                    out.unlink()  # never write through a hard link
                if not out.exists() or out.read_text() != patched[rel]:
                    out.write_text(patched[rel])
                continue
            if same:
                continue
            if out.exists() or out.is_symlink():
                out.unlink()
            try:
                os.link(src, out)
            except OSError:
                shutil.copy2(src, out)


SDK_LIBS = [
    "network/JsonSax",
    "network/Opds",
    "network/CatalogList",
    "ui/FreeInkUI",
    "assets/Icons",
    "font/FreeInkFont",
]

SKIP_DIRS = {"examples", "example", "test", "tests", ".git", "tools", "third_party_tests"}


class Lib:
    def __init__(self, name, src_dir, include_dirs, sources, extra_flags=()):
        self.name = name
        self.src_dir = src_dir
        self.include_dirs = include_dirs
        self.sources = sources
        self.extra_flags = list(extra_flags)


def collect(root, exts=(".c", ".cpp", ".cc", ".S")):
    out = []
    if not root.is_dir():
        return out
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = sorted(d for d in dirnames if d not in SKIP_DIRS)
        for f in sorted(filenames):
            if f.endswith(exts):
                out.append(Path(dirpath) / f)
    return out


def platformio_lib(name, path, srcdir=None, includedir=None, only=None, flags=()):
    """Mimic PlatformIO's default library layout rules."""
    if srcdir:
        src = path / srcdir
    elif (path / "src").is_dir():
        src = path / "src"
    else:
        src = path
    inc = []
    if includedir:
        inc.append(path / includedir)
    elif (path / "include").is_dir():
        inc.append(path / "include")
    inc.append(src)
    if src != path:
        inc.append(path)
    sources = collect(src)
    if only:
        sources = [s for s in sources if s.name in only]
    lib_flags = []
    for f in flags:
        # library.json flags are relative to the library folder
        if f.startswith("-I "):
            lib_flags.append("-I" + str(path / f[3:].strip()))
        else:
            lib_flags.append(f)
    return Lib(name, src, inc, sources, lib_flags)


def gather(fw, deps):
    libs = []
    # 1. This repo: host HAL + Arduino/ESP-IDF shims. Its headers must win
    #    over the SDK's device headers, so it goes first on the include path.
    libs.append(platformio_lib("kindle_hal", HERE))

    # 2. Firmware lib/ (except the device HAL, which the host HAL replaces).
    import json

    for d in sorted((fw / "lib").iterdir()):
        if not d.is_dir() or d.name in ("hal", "README"):
            continue
        meta = {}
        if (d / "library.json").exists():
            meta = json.loads((d / "library.json").read_text()).get("build", {})
        only = None
        if "srcFilter" in meta:
            only = {f[2:-1] for f in meta["srcFilter"] if f.startswith("+<")}
        lib = platformio_lib(d.name, d, meta.get("srcDir"), meta.get("includeDir"), only,
                             meta.get("flags", []))
        if d.name == "Epub":
            # Same as skip_device_content_protection.py: the host build uses
            # the simulator's binding instead of the device crypto binding.
            lib.sources = [s for s in lib.sources if s.name != "ContentProtection.cpp"]
        libs.append(lib)

    # 3. FreeInk SDK libraries the simulator links.
    for rel in SDK_LIBS:
        d = fw / "freeink-sdk" / "libs" / rel
        meta = json.loads((d / "library.json").read_text()).get("build", {})
        libs.append(platformio_lib(d.name, d, meta.get("srcDir"), meta.get("includeDir"), None,
                                   meta.get("flags", [])))

    # 4. Third-party Arduino libraries.
    libs.append(platformio_lib("ArduinoJson", deps / "ArduinoJson"))
    libs.append(platformio_lib("AnimatedGIF", deps / "AnimatedGIF"))
    libs.append(platformio_lib("QRCode", deps / "QRCode"))

    # 5. The firmware application itself.
    app_sources = [s for s in collect(fw / "src")
                   if s.relative_to(fw / "src").as_posix() not in SRC_EXCLUDE]
    libs.append(Lib("app", fw / "src", [fw / "src", fw / "include"], app_sources))
    return libs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--firmware", type=Path, required=True)
    ap.add_argument("--deps", type=Path, required=True)
    ap.add_argument("--sysroot", type=Path, required=True)
    ap.add_argument("--tc", required=True)
    ap.add_argument("--out", type=Path, default=HERE / "build")
    ap.add_argument("--version", default="kindle-pw3-" + (HERE / "VERSION").read_text().strip(),
                    help="version string shown by the firmware (default: from VERSION)")
    ap.add_argument("-j", type=int, default=os.cpu_count() or 4)
    ap.add_argument("-k", action="store_true", help="keep going after compile errors")
    args = ap.parse_args()

    fw = args.firmware.resolve()
    deps = args.deps.resolve()
    sysroot = args.sysroot.resolve()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)

    # Generated sources the PlatformIO pre-scripts would produce. The
    # generators rewrite their outputs every run; put back the old timestamp
    # when the content is unchanged so nothing recompiles needlessly.
    import hashlib
    gen_globs = ["lib/I18n/*.h", "lib/I18n/*.cpp", "src/network/html/*.generated.h"]
    before = {}
    for g in gen_globs:
        for f in fw.glob(g):
            before[f] = (f.stat().st_mtime_ns, hashlib.sha1(f.read_bytes()).digest())
    subprocess.check_call([sys.executable, "scripts/gen_i18n.py", "--strip-unused"], cwd=fw,
                          stdout=subprocess.DEVNULL)
    subprocess.check_call([sys.executable, "scripts/build_html.py"], cwd=fw,
                          stdout=subprocess.DEVNULL)

    for f, (mtime, digest) in before.items():
        if f.exists() and hashlib.sha1(f.read_bytes()).digest() == digest:
            os.utime(f, ns=(mtime, mtime))

    # Compile from a mirror so SOURCE_PATCHES can touch headers too.
    mirror = out / "fw"
    mirror_firmware(fw, mirror)
    fw = mirror
    libs = gather(fw, deps)
    includes = []
    for lib in libs:
        for inc in lib.include_dirs:
            if inc not in includes:
                includes.append(inc)
    extra_incs = [
        fw / "freeink-sdk/libs/network/BleKeyboardHost/include",
        fw / "freeink-sdk/libs/book/ContentProtection/include",
        fw / "freeink-sdk/libs/font/FreeInkFont/third_party/stb",
        fw / "freeink-sdk/libs/font/FreeInkFont/third_party/freetype/include",
        sysroot / "include",
    ]
    common = ["-O2", "-g0", "-fno-strict-aliasing", "-ffunction-sections", "-fdata-sections",
              "-Wno-deprecated-declarations", "-Wno-narrowing", "-Wno-unused-command-line-argument",
              "-Wno-date-time"]
    common += ["-D" + d for d in DEFINES]
    common += ['-DCROSSPOINT_VERSION="%s"' % args.version]
    common += ["-I" + str(i) for i in includes + extra_incs]
    cflags = ["-std=gnu11"] + common
    cxxflags = ["-std=gnu++2a"] + common

    jobs = []
    for lib in libs:
        for src in lib.sources:
            rel = src.resolve()
            obj = out / "obj" / lib.name / (str(abs(hash(str(rel))) % 10**8) + "_" + src.name + ".o")
            is_c = src.suffix == ".c"
            flags = (cflags if is_c else cxxflags) + lib.extra_flags
            cc = args.tc + ("cc" if is_c else "c++")
            jobs.append((lib, src, obj, [cc] + flags + ["-MMD", "-c", str(src), "-o", str(obj)]))

    def up_to_date(obj, cmd):
        dep = obj.with_suffix(".d")
        cmdf = obj.with_suffix(".cmd")
        if not obj.exists() or not dep.exists() or not cmdf.exists():
            return False
        if cmdf.read_text() != " ".join(cmd):
            return False
        t = obj.stat().st_mtime
        text = dep.read_text().replace("\\\n", " ")
        for p in text.split(":", 1)[1].split():
            try:
                if os.stat(p).st_mtime > t:
                    return False
            except FileNotFoundError:
                return False
        return True

    def run(job):
        lib, src, obj, cmd = job
        obj.parent.mkdir(parents=True, exist_ok=True)
        if up_to_date(obj, cmd):
            return job, 0, ""
        p = subprocess.run(cmd, capture_output=True, text=True)
        if p.returncode == 0:
            obj.with_suffix(".cmd").write_text(" ".join(cmd))
        return job, p.returncode, p.stderr

    failed = []
    done = 0
    with futures.ThreadPoolExecutor(max_workers=args.j) as ex:
        for job, rc, err in ex.map(run, jobs):
            done += 1
            if rc != 0:
                failed.append(job)
                print("FAILED %s" % job[1], file=sys.stderr)
                print("\n".join(err.splitlines()[:25]), file=sys.stderr)
                if not args.k:
                    break
    print("compiled %d/%d sources, %d failed" % (done - len(failed), len(jobs), len(failed)))
    if failed:
        sys.exit(1)

    # One archive per library, then link with the app objects directly.
    archives = []
    app_objs = []
    for lib in libs:
        objs = [str(j[2]) for j in jobs if j[0] is lib]
        if not objs:
            continue
        if lib.name in ("app", "kindle_hal"):
            app_objs += objs
            continue
        ar = out / ("lib%s.a" % lib.name)
        if ar.exists():
            ar.unlink()
        subprocess.check_call([args.tc + "ar", "rcs", str(ar)] + objs)
        archives.append(str(ar))

    binary = out / "crosspoint"
    link = ([args.tc + "c++", "-static", "-O2", "-Wl,--gc-sections", "-s", "-o", str(binary)]
            + app_objs + ["-Wl,--start-group"] + archives + ["-Wl,--end-group"]
            + ["-L" + str(sysroot / "lib"), "-lfbink", "-lssl", "-lcrypto", "-lpthread", "-lm"])
    p = subprocess.run(link, capture_output=True, text=True)
    if p.returncode != 0:
        print(p.stderr[-6000:], file=sys.stderr)
        sys.exit(1)
    print("linked %s (%.1f MB)" % (binary, binary.stat().st_size / 1e6))


if __name__ == "__main__":
    main()
