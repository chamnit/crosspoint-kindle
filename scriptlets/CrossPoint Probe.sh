#!/bin/sh
# Name: CrossPoint Probe
# Author: CrossPoint for Kindle
# Icon: icons/probe.png
#
# Scriptlet for modern jailbreaks (no KUAL): collects read-only facts about
# this Kindle (screen, touch, battery, light, cover, sleep) for adding it as
# a CrossPoint model, into crosspoint/probe.txt on the USB drive. Its last
# lines are shown on screen.

for dir in /mnt/us/extensions/crosspoint /mnt/us/kmc/kpm/packages/crosspoint; do
    if [ -f "$dir/bin/probe.sh" ]; then
        exec sh "$dir/bin/probe.sh"
    fi
done
echo "CrossPoint not found."
echo "Copy the zip's extensions/crosspoint folder to the Kindle first."
exit 1
