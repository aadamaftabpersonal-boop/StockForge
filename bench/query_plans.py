#!/usr/bin/env python3
"""Query-plan study on the benchmark dataset (see run_bench.py for how it is built).

Each question is answered by running the SAME statement two ways and measuring:
  * index A/B   ALTER INDEX ... INVISIBLE hides an index from the optimizer without
                dropping it, so data and query are identical and only the plan changes
  * keyset vs OFFSET pagination at depth
  * FULLTEXT vs LIKE '%word%'
Timings: average / p50 / p99 of N executions on one persistent connection after a warm-up.
EXPLAIN ANALYZE output (actual rows and time per plan node) is stored verbatim.

    scripts/dev.sh python3 bench/query_plans.py [--runs 100]
"""
import argparse
import json
import os
import statistics
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(ROOT, "tests", "integration"))
import pymysql  # noqa: E402

import dbtools  # noqa: E402

DB = "stockforge_bench"
COLS = ("p.id, p.sku, p.name, p.description, p.price_cents, p.currency, p.image_url, c.id, c.slug, c.name, "
        "CAST(i.on_hand AS SIGNED) - CAST(i.reserved AS SIGNED), i.low_stock_threshold")
FROM = "FROM products p JOIN categories c ON c.id = p.category_id JOIN inventory i ON i.product_id = p.id"
BASE = f"SELECT {COLS} {FROM} WHERE p.is_active = 1"


def connect():
    return pymysql.connect(host=dbtools.HOST, user="root", password=dbtools.ROOT_PW, database=DB, autocommit=True)


def timed(cur, sql, params, runs):
    for _ in range(10):
        cur.execute(sql, params)
        cur.fetchall()
    ts = []
    for _ in range(runs):
        t = time.perf_counter()
        cur.execute(sql, params)
        cur.fetchall()
        ts.append((time.perf_counter() - t) * 1000)
    ts.sort()
    return {"avg_ms": round(statistics.mean(ts), 3), "p50_ms": round(ts[len(ts) // 2], 3),
            "p99_ms": round(ts[int(len(ts) * 0.99) - 1], 3)}


def explain(cur, sql, params):
    cur.execute("EXPLAIN ANALYZE " + sql, params)
    return "\n".join(r[0] for r in cur.fetchall())


def index_ab(cur, runs, case_id, question, sql, params, index):
    out = {"id": case_id, "question": question, "variants": []}
    for visible in (True, False):
        cur.execute(f"ALTER TABLE products ALTER INDEX {index} {'VISIBLE' if visible else 'INVISIBLE'}")
        cur.execute("ANALYZE TABLE products")
        cur.fetchall()
        name = f"with {index}" if visible else f"without {index} (invisible)"
        out["variants"].append({"name": name, "sql": sql, **timed(cur, sql, params, runs), "plan": explain(cur, sql, params)})
    cur.execute(f"ALTER TABLE products ALTER INDEX {index} VISIBLE")
    return out


def compare(cur, runs, case_id, question, variants):
    out = {"id": case_id, "question": question, "variants": []}
    for name, sql, params in variants:
        out["variants"].append({"name": name, "sql": sql, **timed(cur, sql, params, runs), "plan": explain(cur, sql, params)})
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runs", type=int, default=100)
    runs = ap.parse_args().runs
    conn = connect()
    cur = conn.cursor()
    n = cur.execute("SELECT COUNT(*) FROM products") and cur.fetchone()[0]
    top = cur.execute("SELECT MAX(id) FROM products") and cur.fetchone()[0]
    print(f"dataset: {n:,} products", flush=True)
    cases = []
    try:
        cases.append(index_ab(cur, runs, "price_sorted_browse",
                              "Price-sorted first page with no category filter (migration 002's index)",
                              BASE + " ORDER BY p.price_cents ASC, p.id ASC LIMIT 21", (), "ix_products_active_price"))
        cases.append(index_ab(cur, runs, "category_price_sorted",
                              "Category + price-sorted first page (index from migration 001)",
                              BASE + " AND c.slug = %s ORDER BY p.price_cents ASC, p.id ASC LIMIT 21", ("audio",), "ix_products_cat_price"))
        cases.append(compare(cur, runs, "deep_pagination",
                             f"Page ~{(top - 48) // 2 // 20:,} of the newest-first list: keyset cursor vs OFFSET",
                             [("keyset: WHERE p.id < cursor", BASE + " AND p.id < %s ORDER BY p.id DESC LIMIT 21", (top - (top - 48) // 2,)),
                              ("OFFSET", BASE + " ORDER BY p.id DESC LIMIT 21 OFFSET %s", ((top - 48) // 2,)),
                              ("first page (reference)", BASE + " ORDER BY p.id DESC LIMIT 21", ())]))
        cases.append(compare(cur, runs, "search_two_words",
                             "Search for two words: FULLTEXT vs LIKE '%word%'",
                             [("FULLTEXT MATCH ... IN BOOLEAN MODE", BASE + " AND MATCH(p.name, p.description) AGAINST (%s IN BOOLEAN MODE) ORDER BY p.id DESC LIMIT 21", ("+wireless* +keyboard*",)),
                              ("LIKE '%wireless%' AND LIKE '%keyboard%'", BASE + " AND p.name LIKE %s AND p.name LIKE %s ORDER BY p.id DESC LIMIT 21", ("%wireless%", "%keyboard%")),
                              ("FULLTEXT ordered by relevance, not id (no keyset)", BASE + " AND MATCH(p.name, p.description) AGAINST (%s IN BOOLEAN MODE) ORDER BY MATCH(p.name, p.description) AGAINST (%s IN BOOLEAN MODE) DESC LIMIT 21", ("+wireless* +keyboard*", "+wireless* +keyboard*"))]))
        cases.append(compare(cur, runs, "search_one_common_word",
                             "Search for ONE word that appears in 1/8 of all products (what the load generator's 'common word' does)",
                             [("FULLTEXT ordered by id (what the API does)", BASE + " AND MATCH(p.name, p.description) AGAINST (%s IN BOOLEAN MODE) ORDER BY p.id DESC LIMIT 21", ("+wireless*",)),
                              ("FULLTEXT ordered by relevance", BASE + " AND MATCH(p.name, p.description) AGAINST (%s IN BOOLEAN MODE) ORDER BY MATCH(p.name, p.description) AGAINST (%s IN BOOLEAN MODE) DESC LIMIT 21", ("+wireless*", "+wireless*")),
                              ("LIKE '%wireless%'", BASE + " AND p.name LIKE %s ORDER BY p.id DESC LIMIT 21", ("%wireless%",))]))
        cases.append(compare(cur, runs, "search_rare_word",
                             "Search for a rare token: FULLTEXT vs LIKE",
                             [("FULLTEXT", BASE + " AND MATCH(p.name, p.description) AGAINST (%s IN BOOLEAN MODE) ORDER BY p.id DESC LIMIT 21", ("+%s*" % "9999",)),
                              ("LIKE", BASE + " AND p.name LIKE %s ORDER BY p.id DESC LIMIT 21", ("%9999%",))]))
    finally:
        for ix in ("ix_products_active_price", "ix_products_cat_price"):
            cur.execute(f"ALTER TABLE products ALTER INDEX {ix} VISIBLE")
    out = {"dataset_products": n, "runs_per_variant": runs, "cases": cases}
    os.makedirs(os.path.join(ROOT, "bench", "results"), exist_ok=True)
    json.dump(out, open(os.path.join(ROOT, "bench", "results", "query_plans.json"), "w"), indent=1)
    for c in cases:
        print(f"\n{c['question']}")
        for v in c["variants"]:
            print(f"   {v['name']:<52} avg {v['avg_ms']:>8.3f} ms   p50 {v['p50_ms']:>8.3f}   p99 {v['p99_ms']:>8.3f}")


if __name__ == "__main__":
    main()
