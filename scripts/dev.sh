#!/bin/sh
# Run a command inside the Linux dev image with the repo mounted at /work.
#   scripts/dev.sh make test
# Joins the compose network (if the db is up) and loads .env (gitignored).
set -eu
cd "$(dirname "$0")/.."
IMG=stockforge-dev
docker image inspect "$IMG" >/dev/null 2>&1 || docker build -t "$IMG" -f deploy/Dockerfile.dev deploy
set -- "$@"
EXTRA=""
[ -f .env ] && EXTRA="$EXTRA --env-file .env"
docker network inspect stockforge_default >/dev/null 2>&1 && EXTRA="$EXTRA --network stockforge_default"
# shellcheck disable=SC2086
exec docker run --rm $EXTRA -v "$(pwd)":/work -w /work --cap-add=SYS_PTRACE "$IMG" "$@"
