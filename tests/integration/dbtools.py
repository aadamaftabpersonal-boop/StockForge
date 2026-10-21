"""Test database helpers. Talks to MySQL as root (schema, fixtures, invariant
queries) over ONE PERSISTENT CONNECTION PER THREAD, while the server under test
connects as the least-privilege `sf_app` user, exactly like production.
(An earlier version spawned the `mysql` CLI per query: ~150 ms each, which made
the fault-injection sweeps take 11 minutes.)"""
import glob
import os
import threading
import time

import pymysql
from pymysql.constants import CLIENT

HOST = os.environ.get("SF_DB_HOST", "db")
ROOT_PW = os.environ.get("MYSQL_ROOT_PASSWORD", "")
APP_USER = os.environ.get("SF_DB_USER", "sf_app")
APP_PW = os.environ.get("SF_DB_PASSWORD", "")
TEST_DB = "stockforge_test"
REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

_ready = False
_tl = threading.local()


def _connect(user, password, db):
    return pymysql.connect(host=HOST, user=user, password=password, database=db, autocommit=True,
                           charset="utf8mb4", client_flag=CLIENT.MULTI_STATEMENTS, connect_timeout=10,
                           read_timeout=120)


def _cached(db):
    conns = getattr(_tl, "conns", None)
    if conns is None:
        conns = _tl.conns = {}
    c = conns.get(db)
    if c is None or not c.open:
        c = conns[db] = _connect("root", ROOT_PW, db)
    return c


def _forget(db):
    c = getattr(_tl, "conns", {}).pop(db, None)
    if c is not None:
        try:
            c.close()
        except pymysql.err.Error:
            pass


def _fmt(v):
    if v is None:
        return "NULL"
    if isinstance(v, (bytes, bytearray)):
        return v.hex().upper()
    return str(v)


def _mysql(sql, db=None):
    """Run a (possibly multi-statement) script; returns the rows of the LAST result set as lists of strings."""
    for attempt in range(8):
        try:
            with _cached(db).cursor() as cur:
                cur.execute(sql)
                rows = []
                while True:
                    if cur.description:
                        rows = [[_fmt(v) for v in r] for r in cur.fetchall()]
                    if not cur.nextset():
                        return rows
        except pymysql.err.OperationalError as e:
            code = e.args[0] if e.args else 0
            # Fixtures run concurrently with the server under load; like the server, they must
            # retry when MySQL picks them as a deadlock / lock-timeout victim, or when the
            # cached connection went stale.
            if code in (1213, 1205) and attempt < 7:
                time.sleep(0.05 * (attempt + 1))
                continue
            if code in (2006, 2013, 2003, 0) and attempt < 3:
                _forget(db)
                continue
            raise RuntimeError(f"mysql failed: {e}\nSQL: {sql[:300]}") from e
        except pymysql.err.Error as e:
            raise RuntimeError(f"mysql failed: {e}\nSQL: {sql[:300]}") from e
    raise RuntimeError("unreachable")


def as_app(sql):
    """Run SQL as the application's own least-privilege user (exactly what the server can do).
    Returns (returncode, stdout, stderr) like a CLI would."""
    try:
        c = _connect(APP_USER, APP_PW, TEST_DB)
    except pymysql.err.Error as e:
        return 1, "", str(e)
    try:
        with c.cursor() as cur:
            cur.execute(sql)
            rows = cur.fetchall() if cur.description else []
        return 0, "".join("\t".join(_fmt(v) for v in r) + "\n" for r in rows), ""
    except pymysql.err.Error as e:
        return 1, "", str(e)
    finally:
        c.close()


def q(sql):
    """Run SQL against the test DB, return rows as lists of strings."""
    return _mysql(sql, TEST_DB)


def scalar(sql):
    rows = q(sql)
    return rows[0][0] if rows else None


def make_users(n, tag="u"):
    """Create n customers with live sessions directly in SQL (no Argon2 cost).
    Returns [(user_id, session_cookie, csrf_token), ...]."""
    import base64
    import hashlib
    import uuid

    def tok():
        return base64.urlsafe_b64encode(os.urandom(32)).rstrip(b"=").decode()

    pw = scalar("SELECT password_hash FROM users WHERE id=2")
    run = uuid.uuid4().hex[:8]
    emails = [f"{tag}{run}-{i}@load.test" for i in range(n)]
    q("INSERT INTO users (email,password_hash,display_name) VALUES " +
      ",".join(f"('{e}','{pw}','Load User')" for e in emails))
    ids = [int(r[0]) for r in q(f"SELECT id FROM users WHERE email LIKE '{tag}{run}-%@load.test' ORDER BY id")]
    out, vals = [], []
    for uid in ids:
        cookie, csrf = tok(), tok()
        digest = hashlib.sha256(cookie.encode()).hexdigest()
        vals.append(f"(UNHEX('{digest}'),{uid},'{csrf}',NOW()+INTERVAL 1 DAY)")
        out.append((uid, cookie, csrf))
    q("INSERT INTO sessions (token_hash,user_id,csrf_token,expires_at) VALUES " + ",".join(vals))
    return out


def new_product(stock, price=1000, category=1, active=1):
    """Insert a product with `stock` units (ledger-consistent). Returns product id."""
    import uuid
    sku = "FX-" + uuid.uuid4().hex[:10].upper()
    q(f"INSERT INTO products (sku,category_id,name,description,price_cents,is_active) "
      f"VALUES ('{sku}',{category},'Fixture {sku}','fixture',{price},{active})")
    pid = int(scalar(f"SELECT id FROM products WHERE sku='{sku}'"))
    q(f"INSERT INTO inventory (product_id,on_hand,reserved,low_stock_threshold) VALUES ({pid},{stock},0,2)")
    if stock:
        q(f"INSERT INTO inventory_ledger (product_id,delta_on_hand,delta_reserved,reason) VALUES ({pid},{stock},0,'seed')")
    return pid


def put_cart(uid, lines):
    q(f"DELETE FROM cart_items WHERE user_id={uid}")
    if lines:
        q("INSERT INTO cart_items (user_id,product_id,quantity) VALUES " +
          ",".join(f"({uid},{p},{n})" for p, n in lines))


def restock(pid, n):
    """Ledger-consistent stock increase done directly in SQL."""
    q(f"UPDATE inventory SET on_hand=on_hand+{n} WHERE product_id={pid}")
    q(f"INSERT INTO inventory_ledger (product_id,delta_on_hand,delta_reserved,reason) VALUES ({pid},{n},0,'restock')")


def stock(pid):
    """(on_hand, reserved) as ints."""
    r = q(f"SELECT on_hand,reserved FROM inventory WHERE product_id={pid}")[0]
    return int(r[0]), int(r[1])


# Independent restatement of the invariants (different SQL than src/reconcile.c on purpose).
_INVARIANTS = {
    "reserved<=on_hand": "SELECT COUNT(*) FROM inventory WHERE reserved > on_hand",
    "reserved==sum(held)": "SELECT COUNT(*) FROM inventory i WHERE i.reserved <> "
                           "(SELECT COALESCE(SUM(r.quantity),0) FROM stock_reservations r "
                           " WHERE r.product_id=i.product_id AND r.status='held')",
    "on_hand==sum(ledger)": "SELECT COUNT(*) FROM inventory i WHERE i.on_hand <> "
                            "(SELECT COALESCE(SUM(l.delta_on_hand),0) FROM inventory_ledger l WHERE l.product_id=i.product_id)",
    "reserved==sum(ledger)": "SELECT COUNT(*) FROM inventory i WHERE i.reserved <> "
                             "(SELECT COALESCE(SUM(l.delta_reserved),0) FROM inventory_ledger l WHERE l.product_id=i.product_id)",
    "order total==items": "SELECT COUNT(*) FROM orders o WHERE o.total_cents <> "
                          "(SELECT COALESCE(SUM(oi.unit_price_cents*oi.quantity),0) FROM order_items oi WHERE oi.order_id=o.id)",
    "reservation/order state": "SELECT COUNT(*) FROM stock_reservations r JOIN orders o ON o.id=r.order_id WHERE "
                               "(o.status='PENDING_PAYMENT' AND r.status<>'held') OR "
                               "(o.status IN ('PAID','SHIPPED','DELIVERED') AND r.status<>'committed') OR "
                               "(o.status IN ('CANCELLED','EXPIRED') AND r.status<>'released')",
    "items==reservations": "SELECT COUNT(*) FROM order_items oi WHERE NOT EXISTS (SELECT 1 FROM stock_reservations r "
                           "WHERE r.order_id=oi.order_id AND r.product_id=oi.product_id AND r.quantity=oi.quantity)",
    "one success per paid order": "SELECT COUNT(*) FROM orders o WHERE "
                                  "(o.status IN ('PAID','SHIPPED','DELIVERED') AND (SELECT COUNT(*) FROM payment_attempts p "
                                  " WHERE p.order_id=o.id AND p.status='succeeded')<>1) OR "
                                  "(o.status IN ('PENDING_PAYMENT','EXPIRED') AND EXISTS (SELECT 1 FROM payment_attempts p "
                                  " WHERE p.order_id=o.id AND p.status='succeeded'))",
}


def invariant_violations():
    """{name: count} for every invariant that is broken (empty dict = all hold).
    One round trip (one mysql process) for all checks."""
    union = " UNION ALL ".join(f"SELECT '{name}', ({sql})" for name, sql in _INVARIANTS.items())
    return {name: int(n) for name, n in q(union) if int(n)}


_pw_hash = None


def buyer_with_cart(lines):
    """One round trip: a user with a live session, products and a filled cart.
    lines = [(stock, price_cents, quantity), ...]  ->  ((uid, cookie, csrf), [product ids])"""
    import base64
    import hashlib
    import uuid
    global _pw_hash
    if _pw_hash is None:
        _pw_hash = scalar("SELECT password_hash FROM users WHERE id=2")
    run = uuid.uuid4().hex[:10]
    cookie = base64.urlsafe_b64encode(os.urandom(32)).rstrip(b"=").decode()
    csrf = base64.urlsafe_b64encode(os.urandom(32)).rstrip(b"=").decode()
    digest = hashlib.sha256(cookie.encode()).hexdigest()
    sql = [f"INSERT INTO users (email,password_hash,display_name) VALUES ('b{run}@load.test','{_pw_hash}','Buyer');",
           "SET @uid = LAST_INSERT_ID();",
           f"INSERT INTO sessions (token_hash,user_id,csrf_token,expires_at) "
           f"VALUES (UNHEX('{digest}'),@uid,'{csrf}',NOW()+INTERVAL 1 DAY);"]
    for i, (stock, price, qty) in enumerate(lines):
        sku = f"FX-{run}-{i}"
        sql += [f"INSERT INTO products (sku,category_id,name,description,price_cents) VALUES ('{sku}',1,'Fixture {sku}','x',{price});",
                f"SET @p{i} = LAST_INSERT_ID();",
                f"INSERT INTO inventory (product_id,on_hand,reserved,low_stock_threshold) VALUES (@p{i},{stock},0,2);"]
        if stock:
            sql.append(f"INSERT INTO inventory_ledger (product_id,delta_on_hand,delta_reserved,reason) VALUES (@p{i},{stock},0,'seed');")
        sql.append(f"INSERT INTO cart_items (user_id,product_id,quantity) VALUES (@uid,@p{i},{qty});")
    sql.append("SELECT @uid, " + ", ".join(f"@p{i}" for i in range(len(lines))) + ";")
    row = _mysql("\n".join(sql), TEST_DB)[-1]
    return (int(row[0]), cookie, csrf), [int(x) for x in row[1:]]


def reset_db():
    """Drop and rebuild the test database: migrations, seed, app user grants."""
    global _ready
    if not (ROOT_PW and APP_PW):
        raise RuntimeError("MYSQL_ROOT_PASSWORD and SF_DB_PASSWORD must be set (see .env.example)")
    _forget(TEST_DB)                       # a connection whose default database is dropped is unusable
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
