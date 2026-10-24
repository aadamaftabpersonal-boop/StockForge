#!/bin/sh
# Creates the application database, applies pending migrations, and creates the
# least-privilege app user (SELECT/INSERT/UPDATE/DELETE only; no DDL).
#   scripts/dev.sh sh scripts/db-init.sh            # schema only
#   scripts/dev.sh sh scripts/db-init.sh --seed     # + DEV/DEMO data (never in production!)
# Needs MYSQL_ROOT_PASSWORD, SF_DB_HOST, SF_DB_NAME, SF_DB_USER, SF_DB_PASSWORD (see .env.example).
set -eu
cd "$(dirname "$0")/.."
: "${MYSQL_ROOT_PASSWORD:?}" "${SF_DB_PASSWORD:?}"
HOST="${SF_DB_HOST:-db}"; DB="${SF_DB_NAME:-stockforge}"; USER="${SF_DB_USER:-sf_app}"
export MYSQL_PWD="$MYSQL_ROOT_PASSWORD"
my() { mysql -h"$HOST" -uroot "$@"; }

my -e "CREATE DATABASE IF NOT EXISTS \`$DB\` CHARACTER SET utf8mb4"
for f in db/migrations/*.sql; do
  ver=$(basename "$f" | cut -c1-3)
  done_already=$(my -N "$DB" -e "SELECT COUNT(*) FROM schema_migrations WHERE version='$ver'" 2>/dev/null || echo 0)
  if [ "$done_already" = "0" ]; then echo "applying $f"; my "$DB" < "$f"; else echo "skipping $f (already applied)"; fi
done
my -e "CREATE USER IF NOT EXISTS '$USER'@'%' IDENTIFIED BY '$SF_DB_PASSWORD';
       ALTER USER '$USER'@'%' IDENTIFIED BY '$SF_DB_PASSWORD';
       GRANT SELECT, INSERT, UPDATE, DELETE ON \`$DB\`.* TO '$USER'@'%';"
if [ "${1:-}" = "--seed" ]; then
  if [ "$(my -N "$DB" -e 'SELECT COUNT(*) FROM users')" = "0" ]; then
    echo "loading demo seed + 45 days of demo order history"; my "$DB" < db/seed/001_seed.sql; my "$DB" < db/seed/002_demo_history.sql
  else echo "users exist; not seeding"; fi
fi
echo "database $DB ready"
