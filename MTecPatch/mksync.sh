#!/bin/sh
# Builds MTecSync.adf: the test disk, booting into the read-only sync ladder.
A=${1:-MTecSync.adf}
./mkadf.sh "$A" disk/startup-sync >/dev/null || exit 1
xdftool "$A" write MTecSync C/MTecSync >/dev/null || exit 1
xdftool "$A" info | tail -1
