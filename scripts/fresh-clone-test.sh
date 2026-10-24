#!/bin/sh
# Proves the README works on a clean checkout: clones the repository into a temp directory,
# follows the quick start with generated throwaway passwords, builds, runs the unit tests, a
# slice of the integration tests, then starts the full stack and checks it through Apache.
#
#   scripts/fresh-clone-test.sh [repo-url-or-path]     (default: the origin of this checkout)
#
# Runs on the HOST (needs docker, git, curl). It uses its own compose project ("sffresh"),
# so it does not touch your normal stack's containers or volumes, but it does need host ports
# 8081 and 8443 to be free: run `docker compose ... --profile stack down` on your normal stack first.
# It only sees COMMITTED and PUSHED work: that is the point.
set -eu
SRC="${1:-$(git -C "$(dirname "$0")/.." remote get-url origin)}"
TMP="$(mktemp -d)"
trap 'docker compose -p sffresh --env-file "$TMP/repo/.env" -f "$TMP/repo/deploy/docker-compose.yml" --profile stack down -v >/dev/null 2>&1 || true; rm -rf "$TMP"' EXIT
step() { printf '\n== %s\n' "$*"; }

step "clone $SRC"
git clone --depth 1 "$SRC" "$TMP/repo"
cd "$TMP/repo"

step "configure (random throwaway passwords)"
rnd() { head -c 24 /dev/urandom | od -An -tx1 | tr -d ' \n'; }
sed -e "s/^MYSQL_ROOT_PASSWORD=.*/MYSQL_ROOT_PASSWORD=$(rnd)/" -e "s/^SF_DB_PASSWORD=.*/SF_DB_PASSWORD=$(rnd)/" .env.example > .env
if grep -q 'change-me' .env; then echo "FAIL: a placeholder password survived"; exit 1; fi

COMPOSE="docker compose -p sffresh --env-file .env -f deploy/docker-compose.yml"
step "database up"
$COMPOSE up -d --wait db

# scripts/dev.sh joins network stockforge_default; our project is named sffresh, so run the dev image directly.
step "build dev image"
docker build -t stockforge-dev-fresh -f deploy/Dockerfile.dev deploy >/dev/null
dev() { docker run --rm --env-file .env --network sffresh_default -v "$PWD":/work -w /work stockforge-dev-fresh "$@"; }

step "schema and seed"
dev sh scripts/db-init.sh --seed

step "build and unit tests"
dev sh -c 'make && make test'

step "integration tests (checkout, authz matrix, admin console, demo seed)"
dev sh scripts/it.sh test_checkout test_checkout_concurrency test_authz_matrix test_admin_console test_demo_seed

step "certificate and full stack"
dev sh scripts/gen-dev-cert.sh
$COMPOSE --profile stack up -d --build --wait

step "check through Apache over TLS"
code() { curl -sk -o /dev/null -w '%{http_code}' "$@"; }
[ "$(code https://localhost:8443/)" = 200 ]                       || { echo "FAIL: storefront"; exit 1; }
[ "$(code https://localhost:8443/admin/)" = 200 ]                 || { echo "FAIL: admin page"; exit 1; }
[ "$(code https://localhost:8443/healthz)" = 200 ]                || { echo "FAIL: healthz"; exit 1; }
[ "$(code https://localhost:8443/api/v1/products)" = 200 ]        || { echo "FAIL: products"; exit 1; }
[ "$(code https://localhost:8443/metrics)" = 403 ]                || { echo "FAIL: /metrics must not be public"; exit 1; }
[ "$(code https://localhost:8443/api/v1/admin/stats)" = 401 ]     || { echo "FAIL: admin api must demand a session"; exit 1; }
[ "$(code http://localhost:8081/)" = 301 ] || [ "$(code http://localhost:8081/)" = 302 ] || { echo "FAIL: http must redirect to https"; exit 1; }

step "consistency check on the live demo data"
dev build/rel/reconcile >/dev/null && echo "reconcile: every invariant holds"

printf '\nFRESH CLONE OK: the README works as written.\n'
