#!/bin/sh
# Proves the Valgrind wrapper detects errors: a deliberately broken program must exit 99.
#   scripts/dev.sh sh scripts/valgrind-selftest.sh
set -u
cd "$(dirname "$0")/.."
mkdir -p build
gcc -O0 -g -o build/vg_selftest tests/tools/vg_selftest.c
SF_REAL_BIN=build/vg_selftest sh scripts/valgrind-sf.sh >/dev/null 2>build/vg_selftest.log
rc=$?
grep -E 'uninitialised|definitely lost' build/vg_selftest.log | head -3
if [ "$rc" = "99" ]; then echo "OK: valgrind wrapper reports errors (exit 99)"; else echo "FAIL: expected exit 99, got $rc"; exit 1; fi
