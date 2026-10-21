#!/bin/sh
# Full local verification: release build, unit tests, integration tests.
# Optional: SAN=address,undefined scripts/check.sh   (or SAN=thread)
# Optional: REPEAT=5 scripts/check.sh                (flakiness hunting)
set -eu
cd "$(dirname "$0")/.."
make
make test
n=${REPEAT:-1}
i=0
while [ "$i" -lt "$n" ]; do
  make itest 2>&1 | grep -E '^(Ran|OK|FAILED|ERROR|FAIL)' || true
  i=$((i + 1))
done
