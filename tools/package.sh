#!/bin/sh
# Bundle build/crosspoint and the KUAL extension into a release zip whose
# extensions/ folder is copied onto the Kindle's USB drive as-is:
#   build/crosspoint-kindle-pw3-v<VERSION>.zip
#
# The zip carries the license texts and notices the binary needs (it links
# GPL-3.0-or-later and Apache-2.0 code; see THIRD_PARTY_NOTICES.md) under
# extensions/crosspoint/licenses/. They are copied from this repo, the
# firmware checkout and the Arduino libraries, found next to this repo by
# default (the layout tools/setup.sh creates); override with FIRMWARE=... and
# DEPS=...
set -eu
REPO="$(cd "$(dirname "$0")/.." && pwd)"
FIRMWARE="${FIRMWARE:-$REPO/../crosspoint-reader}"
DEPS="${DEPS:-$REPO/../deps}"
VERSION="$(cat "$REPO/VERSION")"
BIN="$REPO/build/crosspoint"
[ -f "$BIN" ] || { echo "build/crosspoint missing; run tools/build.py first" >&2; exit 1; }
for f in "$FIRMWARE/LICENSE" "$FIRMWARE/licenses" "$DEPS/ArduinoJson/LICENSE.txt" \
         "$DEPS/QRCode/LICENSE.txt" "$DEPS/AnimatedGIF/LICENSE"; do
    [ -e "$f" ] || { echo "missing $f (set FIRMWARE= / DEPS=)" >&2; exit 1; }
done

STAGE="$REPO/build/pkg"
OUT="$REPO/build/crosspoint-kindle-pw3-v$VERSION.zip"
[ -d "$STAGE" ] && find "$STAGE" -mindepth 1 -delete
mkdir -p "$STAGE/extensions"
cp -R "$REPO/kual/crosspoint" "$STAGE/extensions/"
EXT="$STAGE/extensions/crosspoint"
cp "$BIN" "$EXT/bin/crosspoint"
chmod +x "$EXT/bin/"*
cp "$REPO/README.md" "$REPO/CHANGELOG.md" "$EXT/"
mkdir -p "$EXT/docs" && cp "$REPO/docs/"*.svg "$EXT/docs/"  # images the README shows

L="$EXT/licenses"
mkdir -p "$L/firmware" "$L/libraries"
cp "$REPO/LICENSE" "$L/LICENSE"                       # this port + simulator (MIT)
cp "$REPO/THIRD_PARTY_NOTICES.md" "$L/"
cp "$REPO/licenses/"*.txt "$L/"                       # GPL-3.0, Apache-2.0 full texts
cp "$FIRMWARE/LICENSE" "$L/firmware/LICENSE"          # CrossPoint Reader (MIT)
cp "$FIRMWARE/licenses/"* "$L/firmware/"              # its third-party notices (fonts, data, code)
cp "$DEPS/ArduinoJson/LICENSE.txt" "$L/libraries/ArduinoJson-LICENSE.txt"
cp "$DEPS/QRCode/LICENSE.txt" "$L/libraries/QRCode-LICENSE.txt"
cp "$DEPS/AnimatedGIF/LICENSE" "$L/libraries/AnimatedGIF-LICENSE.txt"

rm -f "$OUT"
(cd "$STAGE" && python3 -c "
import os, sys, zipfile
with zipfile.ZipFile(sys.argv[1], 'w', zipfile.ZIP_DEFLATED) as z:
    for root, dirs, files in os.walk('extensions'):
        dirs.sort()
        for f in sorted(files):
            p = os.path.join(root, f)
            info = zipfile.ZipInfo.from_file(p)
            info.compress_type = zipfile.ZIP_DEFLATED
            with open(p, 'rb') as fh:
                z.writestr(info, fh.read())
" "$OUT")
echo "$OUT"
