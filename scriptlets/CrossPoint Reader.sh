#!/bin/sh
# Name: CrossPoint Reader
# Author: CrossPoint for Kindle
# DontUseFBInk
#
# Scriptlet for modern jailbreaks (no KUAL): this file sits in the Kindle's
# documents/ folder, shows up in the Library and starts CrossPoint when
# tapped. The app itself is the extensions/crosspoint folder from the zip
# (or a KPM install). The launcher detaches itself, logs to
# crosspoint/crosspoint.log and restores the Kindle UI when CrossPoint exits.

for dir in /mnt/us/extensions/crosspoint /mnt/us/kmc/kpm/packages/crosspoint; do
    if [ -f "$dir/bin/crosspoint.sh" ]; then
        exec sh "$dir/bin/crosspoint.sh"
    fi
done
mkdir -p /mnt/us/crosspoint
echo "CrossPoint not found: copy the zip's extensions/crosspoint folder to the Kindle" \
    >/mnt/us/crosspoint/crosspoint.log
exit 1
