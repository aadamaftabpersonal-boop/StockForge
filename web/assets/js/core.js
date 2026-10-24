// Shared core for the shop and the admin console.
// Rule: user-controlled text is ONLY ever inserted with textContent (h() below); there is no innerHTML anywhere.

const SVG_NS = "http://www.w3.org/2000/svg";

export function h(tag, attrs, ...kids) {
  const svg = tag.startsWith("svg:");
  const el = svg ? document.createElementNS(SVG_NS, tag.slice(4)) : document.createElement(tag);
  for (const [k, v] of Object.entries(attrs || {})) {
    if (v == null || v === false) continue;
    if (k === "class") el.setAttribute("class", v);
    else if (k.startsWith("on") && typeof v === "function") el.addEventListener(k.slice(2), v);
    else if (k === "value" || k === "checked" || k === "disabled" || k === "selected") el[k] = v;
    else el.setAttribute(k, v === true ? "" : String(v));
  }
  for (const kid of kids.flat(Infinity)) {
    if (kid == null || kid === false) continue;
    el.append(kid instanceof Node ? kid : document.createTextNode(String(kid)));
  }
  return el;
}
export const $ = (sel, root = document) => root.querySelector(sel);
export const clear = (el) => { while (el.firstChild) el.removeChild(el.firstChild); return el; };
export const mount = (el, ...kids) => { clear(el); el.append(...kids.flat(Infinity).filter((k) => k != null && k !== false)); return el; };

const usd = new Intl.NumberFormat("en-US", { style: "currency", currency: "USD" });
export const money = (cents) => usd.format((cents || 0) / 100);
export const int = (n) => new Intl.NumberFormat("en-US").format(n || 0);
export function when(iso, withTime = true) {
  if (!iso) return "";
  const d = new Date(iso);
  return d.toLocaleString(undefined, withTime ? { dateStyle: "medium", timeStyle: "short" } : { dateStyle: "medium" });
}
export function ago(iso) {
  const s = Math.round((Date.now() - new Date(iso).getTime()) / 1000);
  if (s < 60) return "just now";
  if (s < 3600) return `${Math.floor(s / 60)} min ago`;
  if (s < 86400) return `${Math.floor(s / 3600)} h ago`;
  return `${Math.floor(s / 86400)} d ago`;
}

// ------------------------------------------------------------------ API
let csrf = null;
export const session = { user: null };
export class ApiError extends Error {
  constructor(status, body) {
    const e = (body && body.error) || {};
    super(e.message || `Request failed (${status})`);
    this.status = status; this.code = e.code || "ERROR"; this.field = e.field || null; this.detail = e; this.requestId = e.request_id || null;
  }
}
export async function api(path, { method = "GET", body, headers = {}, signal } = {}) {
  const init = { method, credentials: "same-origin", headers: { Accept: "application/json", ...headers }, signal };
  if (body !== undefined) { init.body = JSON.stringify(body); init.headers["Content-Type"] = "application/json"; }
  if (method !== "GET" && csrf) init.headers["X-CSRF-Token"] = csrf;
  let res;
  try { res = await fetch("/api/v1" + path, init); }
  catch (e) { if (e.name === "AbortError") throw e; throw new ApiError(0, { error: { code: "NETWORK", message: "Cannot reach the server. Check your connection and try again." } }); }
  let data = null;
  try { data = await res.json(); } catch { /* empty body */ }
  if (!res.ok) throw new ApiError(res.status, data);
  return data;
}
export async function loadSession() {
  try { const r = await api("/auth/me"); csrf = r.csrf_token; session.user = r.user; }
  catch { csrf = null; session.user = null; }
  return session.user;
}
export async function login(email, password) {
  const r = await api("/auth/login", { method: "POST", body: { email, password } });
  csrf = r.csrf_token; session.user = r.user; return r.user;
}
export async function logout() { try { await api("/auth/logout", { method: "POST" }); } finally { csrf = null; session.user = null; } }

// ------------------------------------------------------------------ UI helpers
export function toast(message, kind = "") {
  let box = $(".toasts");
  if (!box) { box = h("div", { class: "toasts", role: "status", "aria-live": "polite" }); document.body.append(box); }
  const t = h("div", { class: "toast " + kind }, message);
  box.append(t);
  setTimeout(() => t.remove(), kind === "bad" ? 7000 : 3800);
}
export function errorText(e) {
  if (e instanceof ApiError) return e.requestId ? `${e.message} (ref ${e.requestId})` : e.message;
  return "Something went wrong. Try again.";
}
export function confirmDialog({ title, body, confirm = "Confirm", danger = false }) {
  return new Promise((resolve) => {
    const dlg = h("dialog", { "aria-labelledby": "dlg-t" },
      h("div", { class: "dlg-head" }, h("h2", { id: "dlg-t" }, title)),
      h("div", { class: "dlg-body" }, h("p", { class: "muted" }, body)),
      h("div", { class: "dlg-foot" },
        h("button", { class: "btn secondary", onclick: () => dlg.close("no") }, "Cancel"),
        h("button", { class: "btn" + (danger ? " danger" : ""), onclick: () => dlg.close("yes") }, confirm)));
    dlg.addEventListener("close", () => { resolve(dlg.returnValue === "yes"); dlg.remove(); });
    document.body.append(dlg);
    dlg.showModal();
  });
}
export function badge(text, kind = "") { return h("span", { class: "badge " + kind }, text); }
const STATUS = {
  PENDING_PAYMENT: ["Awaiting payment", "warn"], PAID: ["Paid", "info"], SHIPPED: ["Shipped", "info"],
  DELIVERED: ["Delivered", "ok"], CANCELLED: ["Cancelled", "bad"], EXPIRED: ["Expired", ""],
};
export const statusLabel = (s) => (STATUS[s] || [s])[0];
export const statusBadge = (s) => badge(...(STATUS[s] || [s, ""]));

// ------------------------------------------------------------------ theme
export function initTheme(button) {
  const apply = (t) => { document.documentElement.dataset.theme = t; try { localStorage.setItem("sf_theme", t); } catch { /* private mode */ } };
  let t = null;
  try { t = localStorage.getItem("sf_theme"); } catch { /* ignore */ }
  apply(t || (matchMedia("(prefers-color-scheme: dark)").matches ? "dark" : "light"));
  if (button) button.addEventListener("click", () => apply(document.documentElement.dataset.theme === "dark" ? "light" : "dark"));
}

// ------------------------------------------------------------------ stock tally (the signature element)
export function tally(available, threshold, hue) {
  const MAX = 12;
  const out = available <= 0, last = available === 1, low = !out && !last && available <= threshold;
  const cls = "tally" + (out ? " out" : last ? " last" : low ? " low" : "");
  const notches = h("span", { class: "notches", "aria-hidden": "true" });
  const shown = out ? 4 : Math.min(available, MAX);
  for (let i = 0; i < shown; i++) {
    const n = h("span", { class: "notch" + (out ? " empty" : "") });
    if (hue && !low && !last && !out) n.style.setProperty("--hue", hue);
    notches.append(n);
  }
  const text = out ? "Out of stock" : last ? "Last one" : available > MAX ? `${int(available)} in stock` : low ? `Only ${available} left` : `${available} in stock`;
  return h("span", { class: cls, role: "img", "aria-label": text }, notches, h("span", { class: "label" }, text));
}

export const hueVar = (slug) => `var(--hue-${slug || "default"})`;

// ------------------------------------------------------------------ product art plates (procedural SVG, no external images)
function glyph(slug, k) {
  const s = (a) => a * (0.92 + (k % 5) * 0.04);
  const stroke = { fill: "none", stroke: "currentColor", "stroke-width": 3, "stroke-linecap": "round", "stroke-linejoin": "round" };
  const g = (...kids) => h("svg:g", stroke, ...kids);
  const P = (tag, a) => h("svg:" + tag, a);
  switch (slug) {
    case "audio": return g(P("path", { d: `M${60 - s(34)} 88 a${s(34)} ${s(34)} 0 0 1 ${s(68)} 0` }), P("rect", { x: 22, y: 82, width: 14, height: 30, rx: 6, fill: "currentColor" }), P("rect", { x: 84, y: 82, width: 14, height: 30, rx: 6, fill: "currentColor" }));
    case "keyboards-mice": {
      const keys = []; for (let r = 0; r < 3; r++) for (let c = 0; c < 8; c++) keys.push(P("rect", { x: 14 + c * 12.5, y: 62 + r * 14 + (k % 2) * 2, width: 10, height: 10, rx: 2 }));
      return g(P("rect", { x: 8, y: 54, width: 104, height: 54, rx: 7 }), keys, P("rect", { x: 40, y: 98, width: 40, height: 6, rx: 2 }));
    }
    case "home-office": return g(P("path", { d: "M10 70 H110" }), P("path", { d: "M22 70 V112 M98 70 V112" }), P("rect", { x: 34, y: 38, width: 52, height: 28, rx: 3 }), P("path", { d: "M60 66 v4" }));
    case "bags-carry": return g(P("path", { d: "M26 52 h68 l6 58 H20 z" }), P("path", { d: "M44 52 v-10 a16 16 0 0 1 32 0 v10" }), P("path", { d: "M44 78 h32" }));
    case "lighting": return g(P("path", { d: "M30 112 h30" }), P("path", { d: "M45 112 L62 58 L92 34" }), P("path", { d: "M82 28 l22 12 -14 22 z", fill: "currentColor" }), P("circle", { cx: 62, cy: 58, r: 4 }));
    case "cables-power": return g(P("path", { d: "M14 96 C34 40, 50 120, 70 66 S100 44, 106 60" }), P("rect", { x: 6, y: 90, width: 18, height: 14, rx: 3, fill: "currentColor" }), P("rect", { x: 98, y: 50, width: 16, height: 14, rx: 3, fill: "currentColor" }));
    default: return g(P("rect", { x: 30, y: 44, width: 60, height: 52, rx: 8 }));
  }
}
export function plate(product, big = false) {
  const slug = product.category ? product.category.slug : "default";
  const k = product.id || 0;
  const el = h("div", { class: "plate" + (big ? " big" : ""), style: null });
  el.style.setProperty("--hue", hueVar(slug));
  const svg = h("svg:svg", { viewBox: "0 0 120 140", role: "img", "aria-label": `${product.name} illustration`, preserveAspectRatio: "xMidYMid meet" },
    h("svg:circle", { cx: 60 + ((k * 7) % 22) - 11, cy: 74, r: 52 + (k % 4) * 3, fill: "currentColor", opacity: ".10" }),
    glyph(slug, k));
  el.append(svg);
  return el;
}
