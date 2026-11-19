#!/bin/sh
# Launch CrossPoint Reader on a jailbroken Kindle (started from KUAL).
#
# While CrossPoint runs, the Kindle UI (cvm, the Java framework) is paused
# with SIGSTOP so it does not draw over us or react to touches. Whatever
# happens to CrossPoint - normal exit, crash, kill - the cleanup trap resumes
# the framework and puts the screensaver, light and firewall back.
#
# The launcher also stops the Kindle status bar, keeps the screensaver away,
# turns Wi-Fi off (CrossPoint turns it on for File Transfer) and opens the
# firewall for the file transfer ports.
#
# Leave CrossPoint: Home > "Exit CrossPoint..." (it asks first).
#
# Options: --show-log prints the end of the last log on screen; --verbose
# logs every touch.

EXT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
DATA_DIR="${CPK_DATA_DIR:-/mnt/us/crosspoint}"
LOG="$DATA_DIR/crosspoint.log"
BIN="$EXT_DIR/bin/crosspoint"

if [ "$1" = "--show-log" ]; then
    # Print the end of the last log on screen (eips draws text rows).
    eips -c
    if [ -f "$LOG" ]; then
        row=1
        tail -n 38 "$LOG" | cut -c1-60 | while IFS= read -r line; do
            eips 0 "$row" "$line" >/dev/null 2>&1
            row=$((row + 1))
        done
    else
        eips 0 1 "No CrossPoint log yet"
    fi
    exit 0
fi

# KUAL waits for its child; re-run ourselves detached so the menu can close.
if [ "$1" != "--child" ]; then
    mkdir -p "$DATA_DIR"
    nohup "$0" --child "$@" >/dev/null 2>&1 &
    exit 0
fi
shift

mkdir -p "$DATA_DIR"
[ -f "$LOG" ] && mv -f "$LOG" "$LOG.old"
exec >"$LOG" 2>&1
echo "== CrossPoint for Kindle, $(date)"
cat /proc/version 2>/dev/null

if [ ! -x "$BIN" ]; then
    chmod +x "$BIN" 2>/dev/null
fi

# Optional settings: plain NAME=value lines only.
if [ -f "$EXT_DIR/crosspoint.conf" ]; then
    while IFS= read -r line; do
        case "$line" in
            ''|'#'*) continue ;;
            [A-Za-z_]*=*) export "$line" ;;
        esac
    done < "$EXT_DIR/crosspoint.conf"
fi
export CROSSPOINT_SIM_SD="${CROSSPOINT_SIM_SD:-/mnt/us}"
HTTP_PORT="${CROSSPOINT_SIM_HTTP_PORT:-80}"
WS_PORT=$((HTTP_PORT + 1))
[ "$1" = "--verbose" ] && export CPK_DEBUG_TOUCH=1

lipc_get() { lipc-get-prop "$1" "$2" 2>/dev/null; }

CVM_PID="$(pidof cvm)"
AWESOME_PID="$(pidof awesome)"
STATUSBAR_STOPPED=""
OLD_LIGHT="$(lipc_get com.lab126.powerd flIntensity)"
OLD_WIFI="$(lipc_get com.lab126.cmd wirelessEnable)"
APP_PID=""
WATCH_PID=""

cleanup() {
    trap - EXIT INT TERM HUP
    echo "== cleanup"
    if [ -n "$WATCH_PID" ]; then
        kill "$WATCH_PID" 2>/dev/null
        pkill -f "lipc-wait-event -m com.lab126.powerd outOfScreenSaver" 2>/dev/null
    fi
    [ -n "$APP_PID" ] && kill "$APP_PID" 2>/dev/null
    for p in "$HTTP_PORT" "$WS_PORT"; do
        iptables -D INPUT -p tcp --dport "$p" -j ACCEPT 2>/dev/null
    done
    iptables -D INPUT -p udp --dport 8134 -j ACCEPT 2>/dev/null
    lipc-set-prop com.lab126.powerd preventScreenSaver 0 2>/dev/null
    if [ -n "$CPK_WIFI_WAS_TURNED_OFF" ] && [ -n "$OLD_WIFI" ]; then
        lipc-set-prop com.lab126.cmd wirelessEnable "$OLD_WIFI" 2>/dev/null
        echo "Wi-Fi radio restored ($OLD_WIFI)"
    fi
    [ -n "$OLD_LIGHT" ] && lipc-set-prop com.lab126.powerd flIntensity "$OLD_LIGHT" 2>/dev/null
    [ -n "$STATUSBAR_STOPPED" ] && start statusbar >/dev/null 2>&1
    [ -n "$AWESOME_PID" ] && kill -CONT "$AWESOME_PID" 2>/dev/null
    lipc-set-prop com.lab126.pillow disableEnablePillow enable 2>/dev/null
    [ -n "$CVM_PID" ] && kill -CONT "$CVM_PID" 2>/dev/null
    # Make the Kindle UI repaint the whole screen over our last frame.
    sleep 1
    lipc-set-prop com.lab126.appmgrd start app://com.lab126.booklet.home 2>/dev/null
    echo "== done"
}
trap cleanup EXIT
trap 'exit 130' INT TERM HUP

# Pause the Kindle UI: hide the status bar (pillow), stop the separate
# status bar job (it keeps drawing the clock over apps even with pillow
# disabled), pause the window manager, then the framework. Same as KOReader.
lipc-set-prop com.lab126.pillow disableEnablePillow disable 2>/dev/null
if [ -f /etc/upstart/statusbar.conf ] && stop statusbar >/dev/null 2>&1; then
    STATUSBAR_STOPPED=1
    echo "stopped statusbar job"
fi
sleep 1
if [ -n "$AWESOME_PID" ]; then
    kill -STOP "$AWESOME_PID"
    echo "paused awesome ($AWESOME_PID)"
fi
if [ -n "$CVM_PID" ]; then
    kill -STOP "$CVM_PID"
    echo "paused cvm ($CVM_PID)"
else
    echo "warning: cvm not found; the Kindle UI may draw over CrossPoint"
fi
lipc-set-prop com.lab126.powerd preventScreenSaver 1 2>/dev/null

# Wi-Fi draws power even when idle. Turn the radio off while CrossPoint runs;
# CrossPoint turns it on for its network screens (file transfer) and the
# cleanup puts it back as it was. CPK_KEEP_WIFI=1 in crosspoint.conf skips this.
if [ "$CPK_KEEP_WIFI" != "1" ] && [ -n "$OLD_WIFI" ]; then
    lipc-set-prop com.lab126.cmd wirelessEnable 0 2>/dev/null
    export CPK_WIFI_WAS_TURNED_OFF=1
    echo "Wi-Fi radio off (was $OLD_WIFI)"
fi

# Let the Wi-Fi file transfer page and Calibre/discovery through the firewall.
for p in "$HTTP_PORT" "$WS_PORT"; do
    iptables -I INPUT -p tcp --dport "$p" -j ACCEPT 2>/dev/null
done
iptables -I INPUT -p udp --dport 8134 -j ACCEPT 2>/dev/null

cd "$DATA_DIR" || exit 1

"$BIN" &
APP_PID=$!
echo "started CrossPoint ($APP_PID)"

# After the Kindle wakes from its screensaver, ask CrossPoint to redraw.
if command -v lipc-wait-event >/dev/null 2>&1; then
    (
        lipc-wait-event -m com.lab126.powerd outOfScreenSaver 2>/dev/null |
        while read -r _; do
            kill -USR1 "$APP_PID" 2>/dev/null
        done
    ) &
    WATCH_PID=$!
fi

wait "$APP_PID"
STATUS=$?
APP_PID=""
case "$STATUS" in
    0) echo "CrossPoint exited normally" ;;
    3) echo "CrossPoint could not open the screen (start it from the Kindle home screen in portrait)" ;;
    4) echo "CrossPoint found no touchscreen (set CPK_TOUCH_DEVICE in crosspoint.conf)" ;;
    *) echo "CrossPoint exited with status $STATUS" ;;
esac
exit 0
