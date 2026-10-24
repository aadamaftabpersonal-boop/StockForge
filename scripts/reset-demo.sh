#!/bin/sh
# Rebuilds the DEV/DEMO database from scratch: schema, base seed (48 products, demo accounts),
# 45 days of demo order history, then proves the result is consistent with the reconcile checker.
#   scripts/dev.sh sh scripts/reset-demo.sh
# Destroys the contents of $SF_DB_NAME (default: stockforge). Never run against real data.
set -eu
cd "$(dirname "$0")/.."
: "${MYSQL_ROOT_PASSWORD:?}" "${SF_DB_NAME:=stockforge}"
MYSQL_PWD="$MYSQL_ROOT_PASSWORD" mysql -h "${SF_DB_HOST:-db}" -uroot -e "DROP DATABASE IF EXISTS \`$SF_DB_NAME\`"
sh scripts/db-init.sh --seed
make -s build/rel/reconcile >/dev/null 2>&1 || make -s >/dev/null
if build/rel/reconcile >/dev/null; then echo "reconcile: every invariant holds"; else echo "reconcile: VIOLATION"; exit 1; fi
