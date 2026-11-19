#!/bin/sh
# Collect facts about this Kindle for adding it as a CrossPoint model.
# Read-only: it changes no setting and does not pause the Kindle UI.
#
# Writes /mnt/us/crosspoint/probe.txt (the USB drive's crosspoint/ folder).
# Copy that file off over USB and attach it to an issue or send it on.
# Leaves out personal data: only the model part of the serial number is
# kept, and no Wi-Fi network names or account details are read.

EXT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
DATA_DIR="${CPK_DATA_DIR:-/mnt/us/crosspoint}"
OUT="$DATA_DIR/probe.txt"
mkdir -p "$DATA_DIR"

section() { echo; echo "== $*"; }
show() {  # show FILE...: print each readable file as "path: contents"
    for f in "$@"; do
        [ -r "$f" ] && [ -f "$f" ] && printf '%s: %s\n' "$f" "$(head -c 300 "$f" 2>/dev/null | tr '\n' ' ')"
    done
}
lipc() { printf '%s %s = %s\n' "$1" "$2" "$(lipc-get-prop "$1" "$2" 2>&1 | head -c 200)"; }

{
    echo "CrossPoint Kindle probe, $(date)"
    echo "extension: $EXT_DIR, version $(cat "$EXT_DIR/VERSION" 2>/dev/null || echo '?')"

    section "System"
    uname -a
    show /etc/prettyversion.txt /etc/version.txt
    # Model code only (serial prefix); the rest of the serial is left out.
    [ -r /proc/usid ] && echo "serial prefix: $(head -c 6 /proc/usid)"
    grep -E 'model name|Hardware|Processor|CPU part|Features' /proc/cpuinfo | sort -u
    head -n 3 /proc/meminfo
    ls -l /lib/ld-linux*.so* 2>/dev/null  # armhf vs armel userland

    section "Framebuffer (sysfs)"
    for f in /sys/class/graphics/fb0/*; do
        case "$(basename "$f")" in
            name|virtual_size|bits_per_pixel|stride|rotate|mode|modes|pan) show "$f" ;;
        esac
    done

    section "CrossPoint --probe (FBInk + input devices)"
    if [ -x "$EXT_DIR/bin/crosspoint" ] || chmod +x "$EXT_DIR/bin/crosspoint" 2>/dev/null; then
        "$EXT_DIR/bin/crosspoint" --probe 2>&1
        echo "exit code: $?"
    else
        echo "bin/crosspoint missing"
    fi

    section "Input devices (/proc/bus/input/devices)"
    cat /proc/bus/input/devices 2>/dev/null

    section "Battery"
    for d in /sys/class/power_supply/*; do
        [ -d "$d" ] || continue
        show "$d/type" "$d/capacity" "$d/status" "$d/online"
    done
    for f in /sys/devices/system/*battery*/*/battery_capacity \
             /sys/devices/system/*battery*/*/battery_status; do
        show "$f"
    done

    section "Front light"
    for d in /sys/class/backlight/*; do
        [ -d "$d" ] || continue
        show "$d/brightness" "$d/max_brightness" "$d/actual_brightness"
    done
    lipc com.lab126.powerd flIntensity
    lipc com.lab126.powerd flMaxIntensity

    section "Cover (hall sensor) and orientation sensors"
    find /sys/devices/system /sys/devices/platform -maxdepth 4 \
        \( -iname '*hall*' -o -iname '*cover*' \) 2>/dev/null | head -n 20
    for f in /sys/devices/system/*hall*/*hall*/hall_detect \
             /sys/devices/platform/*hall*/hall_detect \
             /sys/devices/platform/*hall*/hall_enable \
             /sys/devices/platform/*hall*/hall_hw_state; do show "$f"; done
    ls /sys/bus/iio/devices 2>/dev/null

    section "Power and sleep"
    show /sys/power/state /sys/power/mem_sleep /sys/power/autosleep /sys/power/wake_lock
    lipc com.lab126.powerd status
    lipc com.lab126.powerd state
    ls /sys/class/rtc 2>/dev/null

    section "Wi-Fi"
    ls /sys/class/net 2>/dev/null
    lipc com.lab126.cmd wirelessEnable

    section "Kindle UI processes"
    for p in cvm awesome pillowd powerd blanket lipc-daemon; do
        printf '%s: %s\n' "$p" "$(pidof "$p" 2>/dev/null)"
    done
    # Every process (pid, name, memory, command line), to find what draws
    # the Kindle UI on firmware without cvm.
    for d in /proc/[0-9]*; do
        pid="${d#/proc/}"
        [ -r "$d/comm" ] || continue
        rss="$(awk '/^VmRSS/ {print $2}' "$d/status" 2>/dev/null)"
        cmd="$(tr '\0' ' ' <"$d/cmdline" 2>/dev/null | cut -c1-120)"
        printf '%6s %-16s %7s kB  %s\n' "$pid" "$(cat "$d/comm" 2>/dev/null)" "${rss:--}" "$cmd"
    done
    # Which processes hold the touchscreen and framebuffer open.
    for d in /proc/[0-9]*; do
        hits="$(ls -l "$d/fd" 2>/dev/null | grep -oE '/dev/(input/event[0-9]+|fb[0-9]+)' | sort -u | tr '\n' ' ')"
        [ -n "$hits" ] && printf 'open: %s %s %s\n' "${d#/proc/}" "$(cat "$d/comm" 2>/dev/null)" "$hits"
    done

    section "Jailbreak tools"
    ls /mnt/us/extensions 2>/dev/null
    for t in fbink eips lipc-set-prop lipc-wait-event evtest; do
        printf '%s: %s\n' "$t" "$(command -v "$t" 2>/dev/null)"
    done
    echo
    echo "== end"
} >"$OUT" 2>&1

# A scriptlet shows our stdout on screen; KUAL does not, so it passes --eips.
echo "Probe saved to crosspoint/probe.txt"
echo "Copy it off over USB."
if [ "$1" = "--eips" ]; then
    eips -c >/dev/null 2>&1
    eips 0 1 "Probe saved to crosspoint/probe.txt" >/dev/null 2>&1
    eips 0 2 "Copy it off over USB." >/dev/null 2>&1
fi
exit 0
