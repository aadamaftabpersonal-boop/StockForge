#!/usr/bin/env python3
"""Dependency-free SVG charts drawn from bench/results/*.json (no matplotlib needed).
    python3 bench/charts.py   ->  bench/charts/*.svg"""
import glob
import json
import os
from html import escape

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
RES = os.path.join(ROOT, "bench", "results")
OUT = os.path.join(ROOT, "bench", "charts")
PALETTE = ["#2563eb", "#d97706", "#059669", "#7c3aed", "#dc2626"]      # distinguishable for common colour-vision types
W, H, L, R, T, B = 720, 400, 70, 24, 46, 92


def nice(v):
    return f"{v:,.0f}" if v >= 100 else f"{v:,.1f}" if v >= 10 else f"{v:,.2f}"


def frame(title, ylabel, ymax, body, xlabels, xpos):
    s = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" role="img" aria-label="{escape(title)}" '
         f'font-family="system-ui, sans-serif" font-size="12">',
         f'<title>{escape(title)}</title><rect width="{W}" height="{H}" fill="#fff"/>',
         f'<text x="{W / 2}" y="24" text-anchor="middle" font-size="15" font-weight="600" fill="#111">{escape(title)}</text>']
    for i in range(6):                                              # horizontal grid + y labels
        y = T + (H - T - B) * (1 - i / 5)
        s.append(f'<line x1="{L}" y1="{y:.1f}" x2="{W - R}" y2="{y:.1f}" stroke="#e5e7eb"/>')
        s.append(f'<text x="{L - 8}" y="{y + 4:.1f}" text-anchor="end" fill="#374151">{nice(ymax * i / 5)}</text>')
    s.append(f'<text transform="translate(16 {(T + H - B) / 2}) rotate(-90)" text-anchor="middle" fill="#374151">{escape(ylabel)}</text>')
    for lab, x in zip(xlabels, xpos):
        s.append(f'<text x="{x:.1f}" y="{H - B + 18}" text-anchor="middle" fill="#374151">{escape(str(lab))}</text>')
    s += body
    return "\n".join(s)


def line_chart(path, title, ylabel, xlabel, xs, series):
    ymax = max(max(v for v in vals if v is not None) for vals in series.values()) * 1.1 or 1
    n = len(xs)
    xpos = [L + (W - L - R) * (i + 0.5) / n for i in range(n)]
    body = []
    for k, (name, vals) in enumerate(series.items()):
        col = PALETTE[k % len(PALETTE)]
        pts = [(x, T + (H - T - B) * (1 - v / ymax)) for x, v in zip(xpos, vals) if v is not None]
        body.append(f'<polyline fill="none" stroke="{col}" stroke-width="2.5" points="{" ".join(f"{x:.1f},{y:.1f}" for x, y in pts)}"/>')
        for (x, y), v in zip(pts, [v for v in vals if v is not None]):
            body.append(f'<circle cx="{x:.1f}" cy="{y:.1f}" r="4" fill="{col}"><title>{escape(name)}: {nice(v)}</title></circle>')
            body.append(f'<text x="{x:.1f}" y="{y - 9:.1f}" text-anchor="middle" font-size="11" fill="{col}">{nice(v)}</text>')
        body.append(f'<rect x="{L + 8 + k * 190}" y="{H - 24}" width="12" height="12" fill="{col}"/>'
                    f'<text x="{L + 26 + k * 190}" y="{H - 14}" fill="#111">{escape(name)}</text>')
    body.append(f'<text x="{(L + W - R) / 2}" y="{H - 46}" text-anchor="middle" fill="#374151">{escape(xlabel)}</text>')
    open(path, "w", encoding="utf-8").write(frame(title, ylabel, ymax, body, xs, xpos) + "\n</svg>\n")


def bar_chart(path, title, ylabel, labels, values, log=False):
    ymax = max(values) * 1.15 or 1
    n = len(labels)
    slot = (W - L - R) / n
    xpos = [L + slot * (i + 0.5) for i in range(n)]
    body = []
    for i, (x, v) in enumerate(zip(xpos, values)):
        h = (H - T - B) * v / ymax
        col = PALETTE[i % 2 if n <= 4 else i % len(PALETTE)]
        body.append(f'<rect x="{x - slot * 0.32:.1f}" y="{H - B - h:.1f}" width="{slot * 0.64:.1f}" height="{h:.1f}" fill="{col}"><title>{escape(labels[i])}: {nice(v)}</title></rect>')
        body.append(f'<text x="{x:.1f}" y="{H - B - h - 6:.1f}" text-anchor="middle" font-size="11" fill="#111">{nice(v)}</text>')
    short = [l if len(l) < 16 else l[:15] + "…" for l in labels]
    open(path, "w", encoding="utf-8").write(frame(title, ylabel, ymax, body, short, xpos) + "\n</svg>\n")


def goodput(r):
    """Successful (2xx) responses per second. Shed 503s, 4xx and failures are NOT throughput."""
    lg = r["loadgen"]
    return sum(v for k, v in lg["status_counts"].items() if k.startswith("2")) / lg["duration_s"]


def load(exp):
    recs = [json.load(open(p)) for p in sorted(glob.glob(os.path.join(RES, f"{exp}__*.json"))) if not p.endswith((".loadgen.json", "invariants.json"))]
    return recs


def main():
    os.makedirs(OUT, exist_ok=True)
    made = []
    w = load("workers")
    if w:
        xs = [r["server_env"]["SF_WORKERS"] for r in w]
        line_chart(f"{OUT}/workers_throughput.svg", "Throughput vs worker threads (browse mix, 64 clients)", "requests / second",
                   "SF_WORKERS", xs, {"successful req/s": [goodput(r) for r in w]})
        line_chart(f"{OUT}/workers_latency.svg", "Latency vs worker threads (browse mix, 64 clients)", "milliseconds",
                   "SF_WORKERS", xs, {"p50": [r["loadgen"]["latency_ms"]["p50"] for r in w], "p99": [r["loadgen"]["latency_ms"]["p99"] for r in w]})
        made += ["workers_throughput", "workers_latency"]
    c = load("concurrency")
    if c:
        xs = [r["threads"] for r in c]
        line_chart(f"{OUT}/concurrency_throughput.svg", "Throughput vs client concurrency (8 workers, queue 64)", "requests / second",
                   "concurrent clients", xs, {"successful req/s": [goodput(r) for r in c]})
        line_chart(f"{OUT}/concurrency_latency.svg", "Latency vs client concurrency (8 workers, queue 64)", "milliseconds",
                   "concurrent clients", xs, {"p50": [r["loadgen"]["latency_ms"]["p50"] for r in c], "p99": [r["loadgen"]["latency_ms"]["p99"] for r in c]})
        made += ["concurrency_throughput", "concurrency_latency"]
    h = load("hot_row")
    if h:
        xs = [r["label"].replace("skus", "").lstrip("0") or "0" for r in h]
        line_chart(f"{OUT}/hot_row_throughput.svg", "Purchase cycles/s vs number of distinct products bought", "completed cycles / second",
                   "distinct products (1 = every buyer wants the same SKU)", xs, {"cycles/s": [r["loadgen"]["purchase_cycles"]["per_s"] for r in h]})
        line_chart(f"{OUT}/hot_row_checkout_latency.svg", "Checkout latency vs number of distinct products", "milliseconds",
                   "distinct products", xs, {"p50": [r["loadgen"]["by_operation"]["checkout"]["p50"] for r in h],
                                             "p99": [r["loadgen"]["by_operation"]["checkout"]["p99"] for r in h]})
        made += ["hot_row_throughput", "hot_row_checkout_latency"]
    qp = os.path.join(RES, "query_plans.json")
    if os.path.exists(qp):
        q = json.load(open(qp))
        for case in q["cases"]:
            bar_chart(f"{OUT}/query_{case['id']}.svg", case["question"][:70], "average ms per query",
                      [v["name"] for v in case["variants"]], [v["avg_ms"] for v in case["variants"]])
            made.append("query_" + case["id"])
    print("charts:", ", ".join(made) or "none (no results yet)")


if __name__ == "__main__":
    main()
