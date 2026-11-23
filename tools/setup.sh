#!/bin/sh
# One-time setup for building CrossPoint for the Kindle Paperwhite 3.
#
# Creates, under $WORK (default: the folder above this repo):
#   kindle-tc/       compiler wrappers around Zig's clang (arm-linux-musleabihf, Cortex-A9)
#   kindle-sysroot/  FBInk (Kindle e-ink library) and OpenSSL, static, for ARM
#   deps/            ArduinoJson, AnimatedGIF, QRCode
#   fonts-src/       six OFL reading fonts from google/fonts (shipped in the zip)
#   crosspoint-reader/  the firmware at the tested revision, unless already present
#
# Every dependency is pinned to the revision this port was built and tested
# with, so a release can be rebuilt exactly (see "Building" in README.md).
#
# Needs: python3 + pip, git, make, perl. Linux or macOS.
# Then build with:  tools/build.py --firmware ../crosspoint-reader --deps ../deps \
#                       --sysroot ../kindle-sysroot --tc ../kindle-tc/kindle-
set -eu

REPO="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${WORK:-$(dirname "$REPO")}"
TC="$WORK/kindle-tc"
SYSROOT="$WORK/kindle-sysroot"
SRC="$WORK/kindle-build-src"
JOBS="${JOBS:-4}"

echo "== Zig toolchain"
python3 -m pip install --user --quiet ziglang==0.17.1 2>/dev/null ||
    python3 -m pip install --break-system-packages --quiet ziglang==0.17.1
mkdir -p "$TC"
TARGET="-target arm-linux-musleabihf -mcpu=cortex_a9"
for tool in cc c++ ar ranlib; do
    case "$tool" in
        cc) args="cc $TARGET" ;;
        c++) args="c++ $TARGET" ;;
        *) args="$tool" ;;
    esac
    printf '#!/bin/sh\nexec python3 -m ziglang %s "$@"\n' "$args" > "$TC/kindle-$tool"
    chmod +x "$TC/kindle-$tool"
done

mkdir -p "$SRC" "$SYSROOT/include" "$SYSROOT/lib"

echo "== FBInk"
FBINK_REV=ded92fa510c957e0157c92ae83bd5c51af710c05
if [ ! -d "$SRC/FBInk" ]; then
    git clone https://github.com/NiLuJe/FBInk "$SRC/FBInk"
    git -C "$SRC/FBInk" checkout -q "$FBINK_REV"
    git -C "$SRC/FBInk" submodule update --init --depth 1 font8x8 stb
fi
mkdir -p "$SRC/fbink-obj"
for f in fbink.c cutef8/utf8.c cutef8/dfa.c; do
    "$TC/kindle-cc" -std=gnu11 -O2 -D_GNU_SOURCE -DFBINK_FOR_KINDLE -DFBINK_MINIMAL \
        -I"$SRC/FBInk" -c "$SRC/FBInk/$f" -o "$SRC/fbink-obj/$(basename "$f" .c).o"
done
rm -f "$SYSROOT/lib/libfbink.a"
"$TC/kindle-ar" rcs "$SYSROOT/lib/libfbink.a" "$SRC"/fbink-obj/*.o
cp "$SRC/FBInk/fbink.h" "$SYSROOT/include/"

echo "== OpenSSL (static; used for hashing and HTTPS)"
if [ ! -f "$SYSROOT/lib/libcrypto.a" ]; then
    [ -d "$SRC/openssl" ] ||
        git clone --depth 1 -b openssl-3.0.15 https://github.com/openssl/openssl "$SRC/openssl"
    (
        cd "$SRC/openssl"
        ./Configure linux-armv4 no-shared no-tests no-asm no-async no-engine no-dso \
            no-afalgeng --prefix="$SYSROOT" --libdir=lib \
            CC="$TC/kindle-cc" AR="$TC/kindle-ar" RANLIB="$TC/kindle-ranlib" >/dev/null
        make -j"$JOBS" build_libs >/dev/null
        make install_dev >/dev/null
    )
fi

echo "== Arduino libraries"
mkdir -p "$WORK/deps"
[ -d "$WORK/deps/ArduinoJson" ] ||
    git clone --depth 1 -b v7.4.2 https://github.com/bblanchon/ArduinoJson "$WORK/deps/ArduinoJson"
if [ ! -d "$WORK/deps/QRCode" ]; then
    git clone https://github.com/ricmoo/QRCode "$WORK/deps/QRCode"
    git -C "$WORK/deps/QRCode" checkout -q eafbde494979abc2445c363cc2602230bcbe299c
fi
if [ ! -d "$WORK/deps/AnimatedGIF" ]; then
    git clone https://github.com/bitbank2/AnimatedGIF "$WORK/deps/AnimatedGIF"
    git -C "$WORK/deps/AnimatedGIF" checkout -q d01888f0255fd0781fc2b8600b9111b14c999584
fi

echo "== Reading fonts (bundled in the zip's fonts/ folder; all SIL OFL 1.1)"
# google/fonts at a fixed commit, only the six families tools/package.sh ships.
FONTS_REV=cc5661d43cd4f9aad5567033a8e15ca28dc35c86
FONT_DIRS="ofl/literata ofl/merriweather ofl/ebgaramond ofl/lora ofl/notoserif ofl/atkinsonhyperlegible"
if [ "$(git -C "$WORK/fonts-src" rev-parse HEAD 2>/dev/null)" != "$FONTS_REV" ]; then
    rm -rf "$WORK/fonts-src"
    git init -q "$WORK/fonts-src"
    git -C "$WORK/fonts-src" remote add origin https://github.com/google/fonts
    git -C "$WORK/fonts-src" sparse-checkout set --no-cone $FONT_DIRS
    git -C "$WORK/fonts-src" fetch -q --depth 1 --filter=blob:none origin "$FONTS_REV"
    git -C "$WORK/fonts-src" checkout -q FETCH_HEAD
fi

echo "== Firmware"
# The revision this port was built and tested against. tools/build.py patches
# a few firmware files by exact text match (SOURCE_PATCHES), so other
# revisions may need those patches updated.
FIRMWARE_REV=6e7caa222819fe20597439f885cb3c4f6bb78567
if [ ! -d "$WORK/crosspoint-reader" ]; then
    git clone https://github.com/crosspoint-reader/crosspoint-reader "$WORK/crosspoint-reader"
    git -C "$WORK/crosspoint-reader" checkout -q "$FIRMWARE_REV"
    git -C "$WORK/crosspoint-reader" submodule update --init --recursive -q
elif [ "$(git -C "$WORK/crosspoint-reader" rev-parse HEAD 2>/dev/null)" != "$FIRMWARE_REV" ]; then
    echo "note: $WORK/crosspoint-reader is not at the tested revision $FIRMWARE_REV"
fi
python3 -m pip install --user --quiet -r "$WORK/crosspoint-reader/requirements.txt" 2>/dev/null ||
    python3 -m pip install --break-system-packages --quiet -r "$WORK/crosspoint-reader/requirements.txt"

echo
echo "Setup done. Build:"
echo "  $REPO/tools/build.py --firmware $WORK/crosspoint-reader --deps $WORK/deps \\"
echo "      --sysroot $SYSROOT --tc $TC/kindle-"
