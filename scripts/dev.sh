#!/bin/sh
# Run a command inside the Linux dev image with the repo mounted at /work.
#   scripts/dev.sh make test
# Build the image on first use.
set -eu
cd "$(dirname "$0")/.."
IMG=stockforge-dev
docker image inspect "$IMG" >/dev/null 2>&1 || docker build -t "$IMG" -f deploy/Dockerfile.dev deploy
exec docker run --rm -v "$(pwd)":/work -w /work --cap-add=SYS_PTRACE "$IMG" "$@"
