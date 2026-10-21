"""Test database helpers. Talks to MySQL with the `mysql` CLI as root (schema,
fixtures, invariant queries) while the server under test connects as the
least-privilege `sf_app` user, exactly like production."""
import glob
import os
import subprocess

HOST = os.environ.get("SF_DB_HOST", "db")
ROOT_PW = os.environ.get("MYSQL_ROOT_PASSWORD", "")
APP_USER = os.environ.get("SF_DB_USER", "sf_app")
APP_PW = os.environ.get("SF_DB_PASSWORD", "")
TEST_DB = "stockforge_test"
REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

_ready = False


def _mysql(sql, db=None):
    env = dict(os.environ, MYSQL_PWD=ROOT_PW)
    cmd = ["mysql", "-h", HOST, "-u", "root", "-N", "-B"] + ([db] if db else [])
    r = subprocess.run(cmd, input=sql, capture_output=True, text=True, env=env)
    if r.returncode:
        raise RuntimeError(f"mysql failed: {r.stderr.strip()}\nSQL: {sql[:300]}")
    return [line.split("\t") for line in r.stdout.splitlines()]


def as_app(sql):
    """Run SQL as the application's own least-privilege user (exactly what the server can do)."""
    env = dict(os.environ, MYSQL_PWD=APP_PW)
    r = subprocess.run(["mysql", "-h", HOST, "-u", APP_USER, "-N", "-B", TEST_DB], input=sql,
                       capture_output=True, text=True, env=env)
    return r.returncode, r.stdout, r.stderr


def q(sql):
    """Run SQL against the test DB, return rows as lists of strings."""
    return _mysql(sql, TEST_DB)


def scalar(sql):
    rows = q(sql)
    return rows[0][0] if rows else None


def reset_db():
    """Drop and rebuild the test database: migrations, seed, app user grants."""
    global _ready
    if not (ROOT_PW and APP_PW):
        raise RuntimeError("MYSQL_ROOT_PASSWORD and SF_DB_PASSWORD must be set (see .env.example)")
    _mysql(f"DROP DATABASE IF EXISTS {TEST_DB}; CREATE DATABASE {TEST_DB} CHARACTER SET utf8mb4;")
    for f in sorted(glob.glob(os.path.join(REPO, "db", "migrations", "*.sql"))):
        _mysql(open(f, encoding="utf-8").read(), TEST_DB)
    _mysql(open(os.path.join(REPO, "db", "seed", "001_seed.sql"), encoding="utf-8").read(), TEST_DB)
    _mysql(
        f"CREATE USER IF NOT EXISTS '{APP_USER}'@'%' IDENTIFIED BY '{APP_PW}';"
        f"ALTER USER '{APP_USER}'@'%' IDENTIFIED BY '{APP_PW}';"
        f"GRANT SELECT, INSERT, UPDATE, DELETE ON {TEST_DB}.* TO '{APP_USER}'@'%';"
    )
    _ready = True


def ensure_ready():
    if not _ready:
        reset_db()
