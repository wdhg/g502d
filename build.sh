#!/bin/bash
# Fail loudly: a broken build used to leave the previous binary in place, and
# anything chained after this script would carry on against stale code.
set -euo pipefail
gcc -o g502d g502d.c -lsystemd -lm -I/usr/include/libevdev-1.0
