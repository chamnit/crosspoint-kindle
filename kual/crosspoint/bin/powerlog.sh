#!/bin/sh
# Power log for the stock Kindle reader, to compare with CrossPoint's own
# "[power]" lines. Logs once a minute for 60 minutes (or MINUTES=n) to
# /mnt/us/crosspoint/power-stock.log, then stops. Starting it again restarts
# it; "--stop" stops it.
#
# Fields: battery %, voltage, current (if the driver reports it), how busy the
# whole system was over the last minute, and the light level. The Kindle may
# sleep during the hour; the log then has a gap.

DATA_DIR="${CPK_DATA_DIR:-/mnt/us/crosspoint}"
OUT="$DATA_DIR/power-stock.log"
PIDFILE="$DATA_DIR/.powerlog.pid"
MINUTES="${MINUTES:-60}"

mkdir -p "$DATA_DIR"
if [ -f "$PIDFILE" ]; then
    kill "$(cat "$PIDFILE")" 2>/dev/null
    rm -f "$PIDFILE"
fi
[ "$1" = "--stop" ] && exit 0

# KUAL waits for its child; run detached.
if [ "$1" != "--child" ]; then
    nohup "$0" --child >/dev/null 2>&1 &
    exit 0
fi
echo $$ >"$PIDFILE"

BAT="$(ls -d /sys/devices/system/*_battery/*_battery0 2>/dev/null | head -n 1)"
LIGHT="$(ls /sys/class/backlight/*/brightness 2>/dev/null | head -n 1)"

val() { [ -n "$1" ] && [ -r "$1" ] && head -n 1 "$1" 2>/dev/null; }

# /proc/stat "cpu" line: prints "total busy" (busy = all but idle and iowait).
cpu() { awk '/^cpu /{t=0; for(i=2;i<=NF;i++) t+=$i; print t, t-$5-$6; exit}' /proc/stat; }

{
    echo "== stock reader power log, $(date), $MINUTES min"
    [ -n "$BAT" ] && echo "battery driver $BAT: $(ls "$BAT" | tr '\n' ' ')"
} >>"$OUT"

set -- $(cpu)
T0=$1 B0=$2
i=0
while [ "$i" -lt "$MINUTES" ]; do
    sleep 60
    i=$((i + 1))
    set -- $(cpu)
    T1=$1 B1=$2
    BUSY="$(awk -v t="$((T1 - T0))" -v b="$((B1 - B0))" 'BEGIN{ if (t>0) printf "%.1f", 100*b/t; else print "?" }')"
    T0=$T1 B0=$B1
    echo "[power] $(date +%H:%M) min $i | battery $(val "$BAT/battery_capacity") $(val "$BAT/battery_voltage") mV $(val "$BAT/battery_current") mA | system busy $BUSY% | light $(val "$LIGHT") | wifi $(lipc-get-prop com.lab126.cmd wirelessEnable 2>/dev/null)" >>"$OUT"
done
echo "== done $(date)" >>"$OUT"
rm -f "$PIDFILE"
