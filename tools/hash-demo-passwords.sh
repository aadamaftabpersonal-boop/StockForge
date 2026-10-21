#!/bin/sh
# Prints Argon2id hashes for the DEV-ONLY demo passwords used by db/seed.
# Run inside the dev image:  scripts/dev.sh sh tools/hash-demo-passwords.sh
set -eu
cd "$(dirname "$0")/.."
make -s hashpw
for pw in 'Admin-Demo-2026!' 'Customer-Demo-2026!'; do
  printf '%s  ' "$pw"
  printf '%s\n' "$pw" | build/rel/hashpw
done
