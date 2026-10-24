import { $, ApiError, api, badge, clear, confirmDialog, errorText, h, initTheme, int, loadSession, login, logout, money, mount, session, statusBadge, statusLabel, tally, toast, when, ago } from "./core.js";

const app = $("#app");
let content = null, timer = null;
const nav = { pills: { orders: 0, products: 0 } };

// ------------------------------------------------------------------ shell and routing
const SECTIONS = [["/", "Overview"], ["/orders", "Orders"], ["/products", "Products"], ["/customers", "Customers"], ["/audit", "Audit log"], ["/system", "System"]];
function parseHash() { const raw = location.hash.replace(/^#/, "") || "/"; const [path, qs] = raw.split("?"); return { path, params: new URLSearchParams(qs || "") }; }
const go = (to) => { location.hash = to; };
const qsOf = (params, extra) => { const p = new URLSearchParams(params); for (const [k, v] of Object.entries(extra)) { if (v) p.set(k, v); else p.delete(k); } const s = p.toString(); return s ? "?" + s : ""; };

function shell(path) {
  const link = ([to, label]) => h("a", { href: "#" + to, "aria-current": (to === "/" ? path === "/" : path.startsWith(to)) ? "page" : null },
    label, to === "/orders" && nav.pills.orders ? h("span", { class: "pill", title: "Paid orders waiting to ship" }, nav.pills.orders) : null,
    to === "/products" && nav.pills.products ? h("span", { class: "pill", title: "Products that are low or out of stock" }, nav.pills.products) : null);
  content = h("main", { id: "main", class: "content", tabindex: "-1" });
  mount(app, h("div", { class: "shell" },
    h("aside", { class: "side" },
      h("a", { class: "brand", href: "#/" }, h("span", { class: "marks", "aria-hidden": "true" }, h("i"), h("i"), h("i")), "StockForge"),
      h("nav", { "aria-label": "Admin sections" }, SECTIONS.map(link)),
      h("div", { class: "foot" }, h("div", {}, session.user.display_name), h("a", { href: "/" }, "Back to the shop"),
        h("div", { class: "row" }, h("button", { class: "btn small secondary", id: "theme", "aria-label": "Toggle dark mode" }, "Theme"),
          h("button", { class: "btn small secondary", onclick: async () => { await logout(); render(); } }, "Sign out")))),
    content));
  initTheme($("#theme"));
}

async function refreshPills() {
  try { const s = await api("/admin/stats"); nav.pills.orders = s.orders_by_status.PAID || 0; nav.pills.products = s.inventory.low_stock + s.inventory.out_of_stock; return s; }
  catch { return null; }
}

const views = { "/": overview, "/orders": ordersView, "/products": productsView, "/customers": customersView, "/audit": auditView, "/system": systemView };
let gen = 0;                                  // a newer navigation supersedes any view still waiting on the network
async function render() {
  clearInterval(timer);
  const my = ++gen;
  await loadSession();
  if (my !== gen) return;
  if (!session.user) return signIn();
  if (session.user.role !== "admin") return mount(app, h("div", { class: "authbox panel box" }, h("h1", {}, "Administrators only"), h("p", {}, "This account doesn't have access to the console."), h("a", { class: "btn", href: "/" }, "Back to the shop"), h("button", { class: "btn secondary", onclick: async () => { await logout(); render(); } }, "Sign in as someone else")));
  const { path, params } = parseHash();
  const view = views[path] || overview;
  const stats = path === "/" ? null : await refreshPills();
  if (my !== gen) return;
  shell(path in views ? path : "/");
  mount(content, h("div", { class: "skeleton skel-h" }), h("div", { class: "skeleton skel-card" }));
  try { await view(params, stats); } catch (e) {
    if (e.status === 401) return render();
    mount(content, h("div", { class: "empty" }, h("h2", {}, "That didn't load"), h("p", {}, errorText(e)), h("button", { class: "btn", onclick: render }, "Try again")));
  }
}
window.addEventListener("hashchange", render);

function signIn() {
  document.title = "Sign in · StockForge admin";
  const err = h("div", { "aria-live": "polite" });
  const email = h("input", { id: "a-email", class: "input", type: "email", autocomplete: "username", required: true, autofocus: true });
  const pw = h("input", { id: "a-pw", class: "input", type: "password", autocomplete: "current-password", required: true });
  const btn = h("button", { class: "btn", type: "submit" }, "Sign in");
  mount(app, h("div", { class: "authbox" }, h("h1", {}, "Admin console"), h("p", { class: "muted" }, "Sign in with an administrator account."),
    h("form", { class: "form", onsubmit: async (e) => {
      e.preventDefault(); btn.disabled = true; clear(err);
      try { await login(email.value, pw.value); render(); }
      catch (ex) { btn.disabled = false; mount(err, h("div", { class: "alert" }, ex.code === "RATE_LIMITED" ? "Too many attempts. Wait a moment and try again." : errorText(ex))); }
    } }, h("div", { class: "field" }, h("label", { for: "a-email" }, "Email"), email), h("div", { class: "field" }, h("label", { for: "a-pw" }, "Password"), pw), btn, err)));
}

// ------------------------------------------------------------------ small building blocks
const pageHead = (title, sub, ...actions) => h("div", { class: "pagehead" }, h("div", {}, h("h1", {}, title), sub && h("p", { class: "muted" }, sub)), h("div", { class: "row" }, ...actions));
const table = (head, rows, empty) => rows.length
  ? h("div", { class: "panel table-wrap" }, h("table", { class: "data" }, h("thead", {}, h("tr", {}, head.map((c) => h("th", { class: typeof c === "object" ? c.cls : "" }, typeof c === "object" ? c.t : c)))), h("tbody", {}, rows)))
  : empty;
const emptyBox = (title, text) => h("div", { class: "panel empty" }, h("h3", {}, title), h("p", {}, text));
function moreButton(next, load, host) { clear(host); if (next) host.append(h("button", { class: "btn secondary", onclick: async (e) => { e.target.disabled = true; try { await load(next); } catch (err) { toast(errorText(err), "bad"); e.target.disabled = false; } } }, "Show more")); }

// A generic form dialog. fields: {name,label,type(text|number|textarea|select|checkbox),value,options,help,required}
function formDialog({ title, intro, fields, submit, onSubmit }) {
  return new Promise((resolve) => {
    const id = "f" + Math.random().toString(36).slice(2, 8);
    const inputs = {}, errs = {};
    const rows = fields.map((f) => {
      const eid = `${id}-${f.name}`;
      errs[f.name] = h("span", { class: "field-error", id: eid + "-e" });
      let el;
      if (f.type === "select") el = h("select", { id: eid, class: "input" }, f.options.map(([v, t]) => h("option", { value: v, selected: String(v) === String(f.value) }, t)));
      else if (f.type === "textarea") el = h("textarea", { id: eid, class: "input", rows: 4, value: f.value || "" });
      else if (f.type === "checkbox") el = h("input", { id: eid, type: "checkbox", checked: !!f.value });
      else el = h("input", { id: eid, class: "input", type: f.type || "text", value: f.value ?? "", required: f.required, autocomplete: "off", min: f.min, max: f.max });
      inputs[f.name] = el;
      if (f.type === "checkbox") return h("label", { class: "check", for: eid }, el, f.label);
      return h("div", { class: "field" }, h("label", { for: eid }, f.label), f.help && h("small", { class: "muted" }, f.help), el, errs[f.name]);
    });
    const alertBox = h("div", { "aria-live": "polite" });
    const dlg = h("dialog", { class: "sheet wide", "aria-labelledby": id + "t" });
    const values = () => Object.fromEntries(fields.map((f) => { const el = inputs[f.name]; return [f.name, f.type === "checkbox" ? el.checked : f.type === "number" ? (el.value === "" ? null : Number(el.value)) : el.value]; }));
    const okBtn = h("button", { class: "btn", type: "submit", form: id }, submit);
    const form = h("form", { class: "form", id, novalidate: true, onsubmit: async (e) => {
      e.preventDefault(); okBtn.disabled = true; clear(alertBox); Object.values(errs).forEach(clear);
      try { await onSubmit(values()); dlg.close("done"); }
      catch (ex) {
        okBtn.disabled = false;
        if (ex instanceof ApiError && ex.field && errs[ex.field]) { errs[ex.field].textContent = ex.message; inputs[ex.field].setAttribute("aria-invalid", "true"); inputs[ex.field].focus(); }
        else mount(alertBox, h("div", { class: "alert" }, errorText(ex)));
      }
    } }, intro && h("p", { class: "muted" }, intro), rows, alertBox);
    dlg.append(h("div", { class: "dlg-head row" }, h("h2", { id: id + "t", class: "grow" }, title), h("button", { class: "btn ghost icon", "aria-label": "Close", onclick: () => dlg.close() }, "✕")),
      h("div", { class: "dlg-body" }, form), h("div", { class: "dlg-foot" }, h("button", { class: "btn secondary", onclick: () => dlg.close() }, "Cancel"), okBtn));
    dlg.addEventListener("close", () => { resolve(dlg.returnValue === "done"); dlg.remove(); });
    dlg.addEventListener("click", (e) => { if (e.target === dlg) dlg.close(); });
    document.body.append(dlg); dlg.showModal();
    const first = Object.values(inputs)[0]; if (first) first.focus();
  });
}

// ------------------------------------------------------------------ overview
function revenueChart(daily, today) {
  const days = [], base = new Date(today + "T00:00:00Z").getTime();
  for (let i = 13; i >= 0; i--) days.push(new Date(base - i * 864e5).toISOString().slice(0, 10));
  const map = Object.fromEntries(daily.map((d) => [d.date, d]));
  const vals = days.map((d) => (map[d] ? map[d].revenue_cents : 0));
  const max = Math.max(...vals, 1), W = 600, H = 190, pad = 26, bw = (W - 10) / days.length;
  const svg = h("svg:svg", { class: "chart", viewBox: `0 0 ${W} ${H + 24}`, role: "img", "aria-label": `Revenue per day for the last 14 days, highest ${money(max)}` });
  for (let g = 0; g <= 3; g++) { const y = pad + ((H - pad) * g) / 3; svg.append(h("svg:line", { class: "grid", x1: 0, x2: W, y1: y, y2: y })); }
  svg.append(h("svg:text", { x: 0, y: 12 }, money(max)));
  vals.forEach((v, i) => {
    const bh = Math.max(v ? 3 : 1, ((H - pad) * v) / max), x = 5 + i * bw;
    svg.append(h("svg:rect", { class: "bar" + (i === days.length - 1 ? " today" : ""), x: x + 2, y: H - bh, width: bw - 4, height: bh, rx: 2 }, h("svg:title", {}, `${days[i]}: ${money(v)}, ${map[days[i]] ? map[days[i]].orders : 0} orders`)));
  });
  [0, 6, 13].forEach((i) => svg.append(h("svg:text", { x: 5 + i * bw + bw / 2, y: H + 18, "text-anchor": i === 0 ? "start" : i === 13 ? "end" : "middle" }, new Date(days[i] + "T00:00:00Z").toLocaleDateString(undefined, { month: "short", day: "numeric", timeZone: "UTC" }))));
  return svg;
}

async function overview() {
  document.title = "Overview · StockForge admin";
  const g = gen;
  const [s, toShip, low] = await Promise.all([api("/admin/stats"), api("/admin/orders?status=PAID&limit=5"), api("/admin/products?stock=low&limit=6")]);
  if (g !== gen) return;
  nav.pills.orders = s.orders_by_status.PAID || 0; nav.pills.products = s.inventory.low_stock + s.inventory.out_of_stock;
  shell("/");
  const cur = s.revenue.last_30d_cents, prev = s.revenue.previous_30d_cents;
  const delta = prev > 0 ? Math.round(((cur - prev) / prev) * 100) : null;
  const order = ["PENDING_PAYMENT", "PAID", "SHIPPED", "DELIVERED", "CANCELLED", "EXPIRED"];
  const total = order.reduce((n, k) => n + (s.orders_by_status[k] || 0), 0) || 1;
  const bar = h("div", { class: "statusbar", role: "img", "aria-label": "Orders by status" });
  order.forEach((k) => { const n = s.orders_by_status[k] || 0; if (n) { const i = h("i", { class: "s-" + k, title: `${statusLabel(k)}: ${n}` }); i.style.setProperty("flex-grow", n); bar.append(i); } });
  const redrawShip = () => overview().catch(() => {});
  mount(content, pageHead("Overview", `Live figures, as of ${when(new Date().toISOString())}.`),
    h("div", { class: "kpis" },
      h("div", { class: "kpi" }, h("span", { class: "muted" }, "Revenue, last 30 days"), h("span", { class: "v" }, money(cur)),
        h("span", { class: "d " + (delta == null ? "" : delta >= 0 ? "up" : "down") }, delta == null ? "No sales in the 30 days before that" : `${delta >= 0 ? "Up" : "Down"} ${Math.abs(delta)}% on the previous 30 days`)),
      h("a", { class: "kpi", href: "#/orders?status=PAID" }, h("span", { class: "muted" }, "Paid, ready to ship"), h("span", { class: "v" }, int(s.orders_by_status.PAID || 0)), h("span", { class: "d" }, "Open the queue")),
      h("a", { class: "kpi", href: "#/products?stock=low" }, h("span", { class: "muted" }, "Low or out of stock"), h("span", { class: "v" }, int(s.inventory.low_stock + s.inventory.out_of_stock)),
        h("span", { class: "d" }, `${s.inventory.out_of_stock} out, ${s.inventory.low_stock} low, of ${s.inventory.active_products} active`)),
      h("a", { class: "kpi", href: "#/customers" }, h("span", { class: "muted" }, "Customers"), h("span", { class: "v" }, int(s.customers.total)), h("span", { class: "d" }, `${s.customers.new_7d} new in the last 7 days`))),
    h("div", { class: "two" },
      h("section", { class: "panel box", "aria-labelledby": "rev" }, h("h2", { id: "rev" }, "Paid revenue by day"), revenueChart(s.daily, s.today)),
      h("section", { class: "panel box", "aria-labelledby": "st" }, h("h2", { id: "st" }, "Orders by status"), bar,
        h("div", { class: "legend" }, order.map((k) => h("a", { href: `#/orders?status=${k}` }, h("span", { class: "sw s-" + k }), statusLabel(k), h("span", { class: "n" }, s.orders_by_status[k] || 0)))))),
    h("div", { class: "three" },
      h("section", { class: "panel box" }, h("h2", {}, "Ready to ship"),
        toShip.items.length ? toShip.items.map((o) => h("div", { class: "list-row" }, h("div", { class: "grow cell-main" }, h("a", { href: "#/orders?status=PAID" }, `${o.id.slice(-8)}, ${o.customer.display_name}`), h("small", {}, `${o.item_count} item${o.item_count === 1 ? "" : "s"}, ${money(o.total_cents)}, ${ago(o.created_at)}`)),
          h("button", { class: "btn small", onclick: async (e) => { e.target.disabled = true; try { await api(`/admin/orders/${o.id}/transition`, { method: "POST", body: { to: "SHIPPED" } }); toast("Marked as shipped"); redrawShip(); } catch (er) { toast(errorText(er), "bad"); e.target.disabled = false; } } }, "Mark shipped")))
          : h("p", { class: "muted" }, "Nothing is waiting. New paid orders appear here.")),
      h("section", { class: "panel box" }, h("h2", {}, "Low on the shelf"),
        low.items.length ? low.items.map((p) => h("div", { class: "list-row" }, h("div", { class: "grow cell-main" }, h("span", {}, p.name), tally(p.available, p.low_stock_threshold, `var(--hue-${p.category.slug})`)),
          h("button", { class: "btn small secondary", onclick: () => adjustStock(p, redrawShip) }, "Restock"))) : h("p", { class: "muted" }, "Every active product is above its low-stock threshold.")),
      h("section", { class: "panel box" }, h("h2", {}, "Best sellers, last 30 days"),
        s.top_products_30d.length ? h("div", { class: "bars" }, s.top_products_30d.map((t) => { const b = h("i"); b.style.setProperty("width", Math.max(6, (t.units / s.top_products_30d[0].units) * 100) + "%"); return h("div", { class: "b" }, h("span", { title: t.name }, `${t.units} units`), h("div", {}, b), h("span", { class: "num" }, money(t.revenue_cents))); })) : h("p", { class: "muted" }, "No paid orders in the last 30 days."),
        s.top_products_30d.length > 0 && h("div", { class: "faint" }, s.top_products_30d.map((t) => t.name).join(", ")))));
}

// ------------------------------------------------------------------ orders
const STATUS_CHIPS = [["", "All"], ["PENDING_PAYMENT", "Awaiting payment"], ["PAID", "Paid"], ["SHIPPED", "Shipped"], ["DELIVERED", "Delivered"], ["CANCELLED", "Cancelled"], ["EXPIRED", "Expired"]];
async function ordersView(params) {
  document.title = "Orders · StockForge admin";
  const status = params.get("status") || "", q = params.get("q") || "";
  const search = h("input", { class: "input", type: "search", id: "oq", placeholder: "Order number or customer email", "aria-label": "Search orders", value: q });
  const body = h("tbody"), more = h("div", { class: "row" }), holder = h("div");
  const load = async (after) => {
    const r = await api("/admin/orders?" + new URLSearchParams({ limit: 25, ...(status && { status }), ...(q && { q }), ...(after && { after }) }));
    if (!after) clear(holder);
    if (!r.items.length && !after) { mount(holder, emptyBox("No orders match", "Try a different status or search.")); return; }
    if (!after) mount(holder, h("div", { class: "panel table-wrap" }, h("table", { class: "data" }, h("thead", {}, h("tr", {}, ["Order", "Customer", "Placed", { t: "Items", cls: "num" }, { t: "Total", cls: "num" }, "Status"].map((c) => h("th", { class: typeof c === "object" ? c.cls : "" }, typeof c === "object" ? c.t : c)))), body)));
    r.items.forEach((o) => { const tr = h("tr", { tabindex: "0", "aria-label": `Open order ${o.id.slice(-8)}`, onclick: () => openOrder(o.id, () => render()), onkeydown: (e) => { if (e.key === "Enter") openOrder(o.id, () => render()); } },
      h("td", {}, h("strong", {}, o.id.slice(-8))), h("td", {}, h("div", { class: "cell-main" }, o.customer.display_name, h("small", {}, o.customer.email))), h("td", {}, when(o.created_at)),
      h("td", { class: "num" }, o.item_count), h("td", { class: "num price" }, money(o.total_cents)), h("td", {}, statusBadge(o.status))); body.append(tr); });
    moreButton(r.next_cursor, load, more);
  };
  mount(content, pageHead("Orders", "Select an order to see its items, payments and history, and to ship or cancel it."),
    h("div", { class: "toolbar" }, h("div", { class: "chips", role: "group", "aria-label": "Filter by status" }, STATUS_CHIPS.map(([v, l]) => h("button", { class: "chip", "aria-pressed": status === v, onclick: () => go("/orders" + qsOf(params, { status: v })) }, l))),
      h("form", { class: "row", onsubmit: (e) => { e.preventDefault(); go("/orders" + qsOf(params, { q: search.value.trim() })); } }, search, h("button", { class: "btn secondary", type: "submit" }, "Search"))), holder, more);
  await load();
}

async function openOrder(id, afterChange) {
  const dlg = h("dialog", { class: "sheet wide", "aria-labelledby": "ot" });
  const body = h("div", { class: "dlg-body stack-lg" }), foot = h("div", { class: "dlg-foot" });
  dlg.append(h("div", { class: "dlg-head row" }, h("h2", { id: "ot", class: "grow" }, `Order ${id.slice(-8)}`), h("button", { class: "btn ghost icon", "aria-label": "Close", onclick: () => dlg.close() }, "✕")), body, foot);
  dlg.addEventListener("close", () => dlg.remove());
  dlg.addEventListener("click", (e) => { if (e.target === dlg) dlg.close(); });
  document.body.append(dlg); dlg.showModal();
  const act = (to, label, danger, confirmText) => h("button", { class: "btn" + (danger ? " danger" : ""), onclick: async () => {
    if (confirmText && !(await confirmDialog({ title: label + "?", body: confirmText, confirm: label, danger }))) return;
    try { await api(`/admin/orders/${id}/transition`, { method: "POST", body: { to } }); toast(to === "SHIPPED" ? "Marked as shipped" : to === "DELIVERED" ? "Marked as delivered" : "Order cancelled"); await draw(); afterChange && afterChange(); }
    catch (e) { toast(errorText(e), "bad"); }
  } }, label);
  const draw = async () => {
    const { order: o } = await api(`/admin/orders/${id}`);
    mount(body, h("div", { class: "row wrap" }, statusBadge(o.status), h("span", { class: "muted" }, `Placed ${when(o.created_at)}`)),
      h("dl", { class: "kv" }, h("dt", {}, "Customer"), h("dd", {}, `${o.customer.display_name}, ${o.customer.email}`), h("dt", {}, "Total"), h("dd", { class: "price" }, money(o.total_cents)), o.expires_at ? [h("dt", {}, "Held until"), h("dd", {}, when(o.expires_at))] : null),
      h("div", { class: "panel" }, o.items.map((it) => h("div", { class: "orderrow" }, h("div", {}, h("strong", {}, it.name), h("div", { class: "muted" }, `${it.sku}, ${it.quantity} × ${money(it.unit_price_cents)}`)), h("span", {}), h("span", { class: "price" }, money(it.line_total_cents))))),
      o.payments.length ? h("div", { class: "stack" }, h("h3", {}, "Payment attempts"), o.payments.map((p) => h("div", { class: "list-row" }, `Attempt ${p.attempt}`, badge(p.status === "succeeded" ? "Succeeded" : p.status === "failed" ? "Declined" : "Timed out", p.status === "succeeded" ? "ok" : "bad"), h("span", { class: "muted grow" }, p.scenario), h("span", { class: "num" }, money(p.amount_cents))))) : null,
      h("div", { class: "stack" }, h("h3", {}, "History"), o.history.map((e) => h("div", { class: "list-row" }, h("span", { class: "grow" }, e.action.replace("order.", "").replace(/_/g, " ")), h("span", { class: "muted" }, e.actor_user_id ? (e.actor_user_id === session.user.id ? "you" : `user ${e.actor_user_id}`) : "system"), h("span", { class: "faint" }, when(e.at))))));
    mount(foot, o.status === "PAID" && act("SHIPPED", "Mark as shipped"), o.status === "SHIPPED" && act("DELIVERED", "Mark as delivered"),
      (o.status === "PENDING_PAYMENT" || o.status === "PAID") && act("CANCELLED", "Cancel order", true, o.status === "PAID" ? "The items go back on the shelf and the payment is reversed (simulated). The customer sees the order as cancelled." : "The reserved items go back on the shelf."));
  };
  try { await draw(); } catch (e) { mount(body, h("div", { class: "alert" }, errorText(e))); }
}

// ------------------------------------------------------------------ products
let categoriesCache = [];
async function productsView(params) {
  document.title = "Products · StockForge admin";
  const stock = params.get("stock") || "", q = params.get("q") || "", cat = params.get("category") || "";
  const g = gen;
  categoriesCache = (await api("/categories")).categories;
  if (g !== gen) return;
  const search = h("input", { class: "input", type: "search", id: "pq", placeholder: "SKU or name", "aria-label": "Search products", value: q });
  const catSel = h("select", { class: "input", "aria-label": "Category", onchange: (e) => go("/products" + qsOf(params, { category: e.target.value })) }, h("option", { value: "" }, "All categories"), categoriesCache.map((c) => h("option", { value: c.slug, selected: c.slug === cat }, c.name)));
  const body = h("tbody"), more = h("div"), holder = h("div");
  const reload = () => render();
  const load = async (after) => {
    const r = await api("/admin/products?" + new URLSearchParams({ limit: 25, ...(stock && { stock }), ...(q && { q }), ...(cat && { category: cat }), ...(after && { after }) }));
    if (!after) { clear(holder); if (!r.items.length) { mount(holder, emptyBox("No products match", "Change the filters, or add a product.")); return; } mount(holder, h("div", { class: "panel table-wrap" }, h("table", { class: "data" }, h("thead", {}, h("tr", {}, ["Product", "Category", { t: "Price", cls: "num" }, "Stock", "On hand / reserved", "Status", ""].map((c) => h("th", { class: typeof c === "object" ? c.cls : "" }, typeof c === "object" ? c.t : c)))), body))); }
    r.items.forEach((p) => body.append(h("tr", {},
      h("td", {}, h("div", { class: "cell-main" }, h("strong", {}, p.name), h("small", {}, p.sku))), h("td", {}, p.category.name), h("td", { class: "num price" }, money(p.price_cents)),
      h("td", {}, tally(p.available, p.low_stock_threshold, `var(--hue-${p.category.slug})`)), h("td", { class: "num" }, `${p.on_hand} / ${p.reserved}`),
      h("td", {}, p.is_active ? badge("Visible", "ok") : badge("Hidden")),
      h("td", {}, h("div", { class: "row" }, h("button", { class: "btn small secondary", onclick: () => adjustStock(p, reload) }, "Stock"), h("button", { class: "btn small secondary", onclick: () => editProduct(p, reload) }, "Edit"))))));
    moreButton(r.next_cursor, load, more);
  };
  mount(content, pageHead("Products", "Prices and visibility apply to new orders only; existing orders keep the price they were placed at.",
    h("button", { class: "btn secondary", onclick: () => manageCategories(reload) }, "Categories"), h("button", { class: "btn", onclick: () => addProduct(reload) }, "Add product")),
    h("div", { class: "toolbar" }, h("div", { class: "chips", role: "group", "aria-label": "Filter by stock" }, [["", "All"], ["low", "Low stock"], ["out", "Out of stock"]].map(([v, l]) => h("button", { class: "chip", "aria-pressed": stock === v, onclick: () => go("/products" + qsOf(params, { stock: v })) }, l))), catSel,
      h("form", { class: "row", onsubmit: (e) => { e.preventDefault(); go("/products" + qsOf(params, { q: search.value.trim() })); } }, search, h("button", { class: "btn secondary", type: "submit" }, "Search"))), holder, more);
  await load();
}
const catOptions = () => categoriesCache.map((c) => [c.id, c.name]);
async function addProduct(done) {
  if (!categoriesCache.length) categoriesCache = (await api("/categories")).categories;
  const ok = await formDialog({ title: "Add a product", submit: "Add product", fields: [
    { name: "sku", label: "SKU", help: "Letters, digits, - and _. Cannot be changed later.", required: true },
    { name: "name", label: "Name", required: true }, { name: "description", label: "Description", type: "textarea" },
    { name: "price", label: "Price in dollars", type: "number", required: true, min: 0.01 }, { name: "category_id", label: "Category", type: "select", options: catOptions() },
    { name: "initial_stock", label: "Units on the shelf now", type: "number", value: 0, min: 0 }],
    onSubmit: (v) => api("/admin/products", { method: "POST", body: { sku: v.sku.trim(), name: v.name.trim(), description: v.description, price_cents: Math.round((v.price || 0) * 100), category_id: Number(v.category_id), initial_stock: v.initial_stock || 0 } }) });
  if (ok) { toast("Product added"); done(); }
}
async function editProduct(p, done) {
  const ok = await formDialog({ title: `Edit ${p.name}`, submit: "Save changes", fields: [
    { name: "name", label: "Name", value: p.name, required: true }, { name: "price", label: "Price in dollars", type: "number", value: (p.price_cents / 100).toFixed(2), min: 0.01 },
    { name: "category_id", label: "Category", type: "select", options: catOptions(), value: p.category.id }, { name: "low_stock_threshold", label: "Warn when this many or fewer are left", type: "number", value: p.low_stock_threshold, min: 0 },
    { name: "is_active", label: "Visible to shoppers", type: "checkbox", value: p.is_active }],
    onSubmit: (v) => api(`/admin/products/${p.id}`, { method: "PATCH", body: { name: v.name.trim(), price_cents: Math.round((v.price || 0) * 100), category_id: Number(v.category_id), low_stock_threshold: v.low_stock_threshold ?? 0, is_active: v.is_active } }) });
  if (ok) { toast("Saved"); done(); }
}
async function adjustStock(p, done) {
  const ok = await formDialog({ title: `Change stock: ${p.name}`, intro: `${p.on_hand} on hand, ${p.reserved} reserved for open orders. Stock cannot drop below the reserved amount.`, submit: "Record change", fields: [
    { name: "delta", label: "Units to add (use a negative number to remove)", type: "number", value: 10, required: true },
    { name: "reason", label: "Reason", type: "select", options: [["restock", "New stock arrived"], ["adjust", "Correction after a count"]] }],
    onSubmit: (v) => { if (!v.delta) throw new ApiError(422, { error: { field: "delta", message: "Enter a number other than zero.", code: "VALIDATION_FAILED" } }); return api(`/admin/inventory/${p.id}/adjust`, { method: "POST", body: { delta: v.delta, reason: v.reason } }); } });
  if (ok) { toast("Stock updated"); done(); }
}
async function manageCategories(done) {
  categoriesCache = (await api("/categories")).categories;
  const dlg = h("dialog", { class: "sheet wide", "aria-labelledby": "ct" }), body = h("div", { class: "dlg-body" });
  const draw = () => mount(body, categoriesCache.map((c) => h("div", { class: "list-row" }, h("div", { class: "grow cell-main" }, h("strong", {}, c.name), h("small", {}, `${c.slug}, ${c.product_count} products`)),
    h("button", { class: "btn small secondary", onclick: async () => { dlg.close(); if (await formDialog({ title: "Rename category", submit: "Rename", fields: [{ name: "name", label: "Name", value: c.name, required: true }], onSubmit: (v) => api(`/admin/categories/${c.id}`, { method: "PATCH", body: { name: v.name.trim() } }) })) { toast("Category renamed"); done(); } } }, "Rename"))));
  dlg.append(h("div", { class: "dlg-head row" }, h("h2", { id: "ct", class: "grow" }, "Categories"), h("button", { class: "btn ghost icon", "aria-label": "Close", onclick: () => dlg.close() }, "✕")), body,
    h("div", { class: "dlg-foot" }, h("button", { class: "btn", onclick: async () => { dlg.close(); if (await formDialog({ title: "Add a category", submit: "Add category", fields: [{ name: "slug", label: "Slug", help: "Lowercase letters, digits and -. Used in links; cannot be changed later.", required: true }, { name: "name", label: "Name", required: true }], onSubmit: (v) => api("/admin/categories", { method: "POST", body: { slug: v.slug.trim(), name: v.name.trim() } }) })) { toast("Category added"); done(); } } }, "Add category")));
  dlg.addEventListener("close", () => dlg.remove()); draw(); document.body.append(dlg); dlg.showModal();
}

// ------------------------------------------------------------------ customers
async function customersView(params) {
  document.title = "Customers · StockForge admin";
  const q = params.get("q") || "";
  const search = h("input", { class: "input", type: "search", id: "cq", placeholder: "Email starts with", "aria-label": "Search customers", value: q });
  const body = h("tbody"), more = h("div"), holder = h("div");
  const load = async (after) => {
    const r = await api("/admin/users?" + new URLSearchParams({ limit: 25, ...(q && { q }), ...(after && { after }) }));
    if (!after) { clear(holder); if (!r.items.length) { mount(holder, emptyBox("No customers match", "Search by the start of an email address.")); return; } mount(holder, h("div", { class: "panel table-wrap" }, h("table", { class: "data" }, h("thead", {}, h("tr", {}, ["Customer", "Joined", { t: "Orders", cls: "num" }, { t: "Spent", cls: "num" }, "Status", ""].map((c) => h("th", { class: typeof c === "object" ? c.cls : "" }, typeof c === "object" ? c.t : c)))), body))); }
    r.items.forEach((u) => body.append(h("tr", {}, h("td", {}, h("div", { class: "cell-main" }, h("strong", {}, u.display_name), h("small", {}, u.email))), h("td", {}, when(u.created_at, false)), h("td", { class: "num" }, u.order_count), h("td", { class: "num price" }, money(u.total_spent_cents)),
      h("td", {}, u.role === "admin" ? badge("Administrator", "info") : u.is_active ? badge("Active", "ok") : badge("Disabled", "bad")),
      h("td", {}, u.role === "admin" || u.id === session.user.id ? null : h("button", { class: "btn small secondary", onclick: async () => {
        if (u.is_active && !(await confirmDialog({ title: `Disable ${u.display_name}?`, body: "They are signed out everywhere immediately and cannot sign in until you enable the account again. Their orders are kept.", confirm: "Disable account", danger: true }))) return;
        try { await api(`/admin/users/${u.id}`, { method: "PATCH", body: { is_active: !u.is_active } }); toast(u.is_active ? "Account disabled" : "Account enabled"); render(); } catch (e) { toast(errorText(e), "bad"); } } }, u.is_active ? "Disable" : "Enable")))));
    moreButton(r.next_cursor, load, more);
  };
  mount(content, pageHead("Customers", "Disabling an account ends its sessions at once."), h("div", { class: "toolbar" }, h("form", { class: "row", onsubmit: (e) => { e.preventDefault(); go("/customers" + qsOf(params, { q: search.value.trim() })); } }, search, h("button", { class: "btn secondary", type: "submit" }, "Search"))), holder, more);
  await load();
}

// ------------------------------------------------------------------ audit log
const AUDIT_ACTIONS = ["order.created", "order.paid", "order.payment_failed", "order.shipped", "order.delivered", "order.cancelled", "order.expired", "product.created", "product.updated", "inventory.adjusted", "category.created", "category.renamed", "user.registered", "user.disabled", "user.enabled", "auth.login", "auth.login_failed"];
async function auditView(params) {
  document.title = "Audit log · StockForge admin";
  const action = params.get("action") || "";
  const sel = h("select", { class: "input", "aria-label": "Filter by action", onchange: (e) => go("/audit" + qsOf(params, { action: e.target.value })) }, h("option", { value: "" }, "All actions"), AUDIT_ACTIONS.map((a) => h("option", { value: a, selected: a === action }, a)));
  const body = h("tbody"), more = h("div"), holder = h("div");
  const load = async (after) => {
    const r = await api("/admin/audit?" + new URLSearchParams({ limit: 50, ...(action && { action }), ...(after && { after }) }));
    if (!after) { clear(holder); if (!r.items.length) { mount(holder, emptyBox("No events match", "Pick a different action.")); return; } mount(holder, h("div", { class: "panel table-wrap" }, h("table", { class: "data" }, h("thead", {}, h("tr", {}, ["When", "Action", "Subject", "By", "Request", "Details"].map((c) => h("th", {}, c)))), body))); }
    r.items.forEach((e) => { const det = e.details ? JSON.stringify(e.details) : ""; body.append(h("tr", {}, h("td", {}, when(e.created_at)), h("td", {}, h("strong", {}, e.action)), h("td", {}, `${e.entity_type} ${e.entity_id.length > 12 ? e.entity_id.slice(-8) : e.entity_id}`),
      h("td", {}, e.actor_user_id ? `user ${e.actor_user_id}` : "system"), h("td", { class: "faint" }, e.request_id || ""), h("td", { class: "json", title: det }, det))); });
    moreButton(r.next_cursor, load, more);
  };
  mount(content, pageHead("Audit log", "Every sign-in, order change and admin action, newest first. Details never contain passwords or tokens."), h("div", { class: "toolbar" }, sel), holder, more);
  await load();
}

// ------------------------------------------------------------------ system
async function systemView() {
  document.title = "System · StockForge admin";
  const host = h("div", { class: "stack-lg" }), recon = h("div");
  mount(content, pageHead("System", "Health, load and consistency of the running server. Refreshes every 5 seconds.",
    h("button", { class: "btn", onclick: async (e) => { e.target.disabled = true; try { const r = await api("/admin/reconcile"); mount(recon, reconcilePanel(r)); } catch (er) { toast(errorText(er), "bad"); } e.target.disabled = false; } }, "Run consistency check")), host, recon);
  const draw = async () => {
    const [m, hz] = await Promise.all([api("/admin/metrics"), fetch("/healthz").then((r) => r.json()).catch(() => null)]);
    const pct = (a, b) => (b ? Math.round((a / b) * 100) : 0);
    const r = m.responses, n = m.requests_total || 1, hist = m.latency_ms_hist;
    const maxH = Math.max(...hist.count, 1);
    mount(host,
      h("div", { class: "kpis" },
        h("div", { class: "kpi" }, h("span", { class: "muted" }, "Server"), h("span", { class: "v" }, hz && hz.status === "ok" ? "Healthy" : "Degraded"), h("span", { class: "d" }, hz ? `Database ${hz.database}, up ${Math.floor(hz.uptime_s / 60)} min` : "Health check unreachable")),
        h("div", { class: "kpi" }, h("span", { class: "muted" }, "Workers busy"), h("span", { class: "v" }, `${m.workers.busy} / ${m.workers.total}`), h("span", { class: "d" }, `${pct(m.workers.busy, m.workers.total)}% utilised`)),
        h("div", { class: "kpi" }, h("span", { class: "muted" }, "Request queue"), h("span", { class: "v" }, `${m.queue.depth} / ${m.queue.capacity}`), h("span", { class: "d" }, `High-water mark ${m.queue.high_water}; ${int(m.connections_rejected_overload)} shed with 503`)),
        h("div", { class: "kpi" }, h("span", { class: "muted" }, "Average request"), h("span", { class: "v" }, `${(m.latency_us_sum / n / 1000).toFixed(1)} ms`), h("span", { class: "d" }, `${(m.queue_wait_us_sum / n / 1000).toFixed(1)} ms of that waiting for a worker`))),
      h("div", { class: "three" },
        h("section", { class: "panel box" }, h("h2", {}, "Requests since start"), h("dl", { class: "kv" }, h("dt", {}, "Total"), h("dd", {}, int(m.requests_total)), h("dt", {}, "Succeeded (2xx)"), h("dd", {}, int(r["2xx"])), h("dt", {}, "Client errors (4xx)"), h("dd", {}, int(r["4xx"])), h("dt", {}, "Server errors (5xx)"), h("dd", {}, int(r["5xx"])), h("dt", {}, "Connections accepted"), h("dd", {}, int(m.connections_accepted)))),
        h("section", { class: "panel box" }, h("h2", {}, "Database"), h("dl", { class: "kv" }, h("dt", {}, "Statements"), h("dd", {}, int(m.db.statements)), h("dt", {}, "Average time"), h("dd", {}, `${(m.db.time_us / Math.max(1, m.db.statements) / 1000).toFixed(2)} ms`), h("dt", {}, "Slow statements"), h("dd", {}, int(m.db.slow)), h("dt", {}, "Connections opened"), h("dd", {}, int(m.db.connects)), h("dt", {}, "Errors"), h("dd", {}, int(m.db.errors)))),
        h("section", { class: "panel box" }, h("h2", {}, "Orders since start"), h("dl", { class: "kv" }, Object.entries({ created: "Created", replayed: "Retries answered from record", idempotency_conflicts: "Key reused wrongly", insufficient_stock: "Refused: not enough stock", deadlock_retries: "Deadlock retries", retries_exhausted: "Retries exhausted", paid: "Paid", payment_failed: "Payments failed", cancelled: "Cancelled", expired: "Expired" }).flatMap(([k, l]) => [h("dt", {}, l), h("dd", {}, int(m.orders[k]))])))),
      h("section", { class: "panel box" }, h("h2", {}, "How long requests took"), h("div", { class: "bars" }, hist.le.map((le, i) => { const b = h("i"); b.style.setProperty("width", Math.max(hist.count[i] ? 2 : 0, (hist.count[i] / maxH) * 100) + "%"); return h("div", { class: "b" }, h("span", {}, le === "+Inf" ? "over 1 s" : `≤ ${le} ms`), h("div", {}, b), h("span", { class: "num" }, int(hist.count[i]))); }))));
  };
  const g = gen;
  await draw();
  if (g !== gen) return;
  timer = setInterval(() => { if (document.hidden) return; draw().catch(() => {}); }, 5000);
}
function reconcilePanel(r) {
  return h("section", { class: "panel box" }, h("div", { class: "row" }, h("h2", { class: "grow" }, "Consistency check"), r.ok ? badge("Every invariant holds", "ok") : badge("Violations found", "bad")),
    r.checks.map((c) => h("div", { class: "check-row" }, badge(c.ok ? "Pass" : "Fail", c.ok ? "ok" : "bad"), h("div", {}, h("div", {}, c.description), !c.ok && h("div", { class: "field-error" }, `Offending ids: ${c.ids.join(", ")}`)))));
}

render();
