#!/bin/bash
# Fail loudly: a broken build used to leave the previous binary in place, and
# anything chained after this script would carry on against stale code.
set -euo pipefail
gcc -D_GNU_SOURCE -o moused moused.c -lsystemd -lm -I/usr/include/libevdev-1.0
