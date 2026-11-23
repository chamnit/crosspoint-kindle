#!/bin/sh
# Bundle build/crosspoint and the KUAL extension into a release zip whose
# extensions/, documents/ and fonts/ folders (the app, Library scriptlets for
# jailbreaks without KUAL, six OFL reading fonts) are copied onto the Kindle's
# USB drive as-is:
#   build/crosspoint-kindle-v<VERSION>.zip
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
FONTS="${FONTS:-$REPO/../fonts-src}"
VERSION="$(cat "$REPO/VERSION")"
BIN="$REPO/build/crosspoint"
[ -f "$BIN" ] || { echo "build/crosspoint missing; run tools/build.py first" >&2; exit 1; }
for f in "$FIRMWARE/LICENSE" "$FIRMWARE/licenses" "$DEPS/ArduinoJson/LICENSE.txt" \
         "$DEPS/QRCode/LICENSE.txt" "$DEPS/AnimatedGIF/LICENSE" "$FONTS/ofl"; do
    [ -e "$f" ] || { echo "missing $f (set FIRMWARE= / DEPS= / FONTS=)" >&2; exit 1; }
done

STAGE="$REPO/build/pkg"
OUT="$REPO/build/crosspoint-kindle-v$VERSION.zip"
[ -d "$STAGE" ] && find "$STAGE" -mindepth 1 -delete
mkdir -p "$STAGE/extensions" "$STAGE/documents"  # fonts/ below
cp -R "$REPO/kual/crosspoint" "$STAGE/extensions/"
# Library entries for jailbreaks without KUAL. Their "# Icon: icons/x.png"
# line becomes the PNG itself (a data: URI), so the icon shows wherever the
# app is installed.
for f in "$REPO/scriptlets/"*.sh; do
    python3 - "$f" "$STAGE/documents/$(basename "$f")" "$REPO/scriptlets" <<'PY'
import base64, os, re, sys
src, dst, base = sys.argv[1:4]
def embed(m):
    data = open(os.path.join(base, m.group(1)), "rb").read()
    return "# Icon: data:image/png;base64," + base64.b64encode(data).decode()
text = re.sub(r"^# Icon: (icons/[\w.-]+\.png)$", embed, open(src).read(), flags=re.M)
open(dst, "w").write(text)
PY
done
EXT="$STAGE/extensions/crosspoint"
cp "$BIN" "$EXT/bin/crosspoint"
chmod +x "$EXT/bin/"*
cp "$REPO/README.md" "$REPO/CHANGELOG.md" "$REPO/VERSION" "$EXT/"
mkdir -p "$EXT/docs" && cp "$REPO/docs/"*.svg "$EXT/docs/"  # images the README shows

# Reading fonts (tools/setup.sh pins them), copied to the drive's fonts/
# folder, where CrossPoint finds them: "Folder name" <- google/fonts ofl/<dir>.
# Each family keeps its OFL.txt, as the license requires.
mkdir -p "$STAGE/fonts"
while IFS=: read -r name dir; do
    mkdir -p "$STAGE/fonts/$name"
    cp "$FONTS/ofl/$dir/"*.ttf "$FONTS/ofl/$dir/OFL.txt" "$STAGE/fonts/$name/"
done <<'EOF_FONTS'
Literata:literata
Merriweather:merriweather
EB Garamond:ebgaramond
Lora:lora
Noto Serif:notoserif
Atkinson Hyperlegible:atkinsonhyperlegible
EOF_FONTS

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
    for top in ('documents', 'extensions', 'fonts'):
        for root, dirs, files in os.walk(top):
            dirs.sort()
            for f in sorted(files):
                p = os.path.join(root, f)
                info = zipfile.ZipInfo.from_file(p)
                info.compress_type = zipfile.ZIP_DEFLATED
                with open(p, 'rb') as fh:
                    z.writestr(info, fh.read())
" "$OUT")
echo "$OUT"
