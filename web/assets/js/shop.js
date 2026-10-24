import { $, ApiError, api, ago, badge, clear, confirmDialog, errorText, h, hueVar, initTheme, int, loadSession, login, logout, money, mount, plate, session, statusBadge, statusLabel, tally, toast, when } from "./core.js";

const main = $("#main");
const state = { categories: [], cart: null, cartOpen: false };

// ------------------------------------------------------------------ routing
function parseHash() {
  const raw = location.hash.replace(/^#/, "") || "/";
  const [path, qs] = raw.split("?");
  return { path, params: new URLSearchParams(qs || "") };
}
const go = (to) => { location.hash = to; };
const routes = [
  [/^\/$/, catalogView], [/^\/p\/(\d+)$/, productView], [/^\/checkout$/, checkoutView], [/^\/orders$/, ordersView],
  [/^\/orders\/([0-9A-Z]{26})$/, orderView], [/^\/login$/, (r) => authView(r, "login")], [/^\/register$/, (r) => authView(r, "register")],
];
async function route() {
  const { path, params } = parseHash();
  renderTopbar();
  for (const [re, view] of routes) {
    const m = path.match(re);
    if (!m) continue;
    mount(main, skeletonPage());
    try { await view({ args: m.slice(1), params }); }
    catch (e) { if (e.name !== "AbortError") mount(main, errorPanel(e)); }
    main.focus({ preventScroll: true });
    window.scrollTo(0, 0);
    return;
  }
  mount(main, h("div", { class: "empty" }, h("h1", {}, "Page not found"), h("a", { class: "btn", href: "#/" }, "Back to the shop")));
}
const skeletonPage = () => h("div", { class: "stack-lg", "aria-busy": "true" }, h("div", { class: "skeleton skel-h" }), h("div", { class: "grid" }, ...Array.from({ length: 6 }, () => h("div", { class: "skeleton skel-card" }))));
const errorPanel = (e) => h("div", { class: "empty" }, h("h2", {}, e.status === 503 ? "The store is temporarily unavailable" : "That didn't load"), h("p", {}, errorText(e)),
  h("button", { class: "btn", onclick: route }, "Try again"));
const needLogin = () => { const next = encodeURIComponent(location.hash.slice(1) || "/"); go(`/login?next=${next}`); };

// ------------------------------------------------------------------ chrome
function renderTopbar() {
  const { path, params } = parseHash();
  const search = h("input", { class: "input", type: "search", placeholder: "Search products", "aria-label": "Search products", value: params.get("q") || "", autocomplete: "off" });
  const form = h("form", { class: "searchbox", role: "search", onsubmit: (e) => { e.preventDefault(); const q = search.value.trim(); go(q ? `/?q=${encodeURIComponent(q)}` : "/"); } }, search);
  const nav = h("nav", { "aria-label": "Main" },
    h("a", { href: "#/", "aria-current": path === "/" ? "page" : null }, "Shop"),
    session.user && h("a", { href: "#/orders", "aria-current": path.startsWith("/orders") ? "page" : null }, "Your orders"),
    session.user && session.user.role === "admin" && h("a", { href: "/admin/" }, "Admin console"));
  const count = state.cart ? state.cart.items.reduce((n, i) => n + i.quantity, 0) : 0;
  const cartBtn = h("button", { class: "btn secondary", onclick: openCart, "aria-label": `Cart, ${count} item${count === 1 ? "" : "s"}` }, "Cart", h("span", { class: "cartcount" }, count));
  const theme = h("button", { class: "btn ghost icon", "aria-label": "Toggle dark mode", id: "theme" }, "◐");
  const who = session.user
    ? h("button", { class: "btn ghost", onclick: async () => { await logout(); state.cart = null; toast("Signed out"); go("/"); route(); } }, `Sign out ${session.user.display_name.split(" ")[0]}`)
    : h("a", { class: "btn", href: "#/login" }, "Sign in");
  mount($("#topbar"), h("a", { class: "brand", href: "#/" }, h("span", { class: "marks", "aria-hidden": "true" }, h("i"), h("i"), h("i")), "StockForge"), nav, form, h("span", { class: "grow" }), theme, cartBtn, who);
  initTheme($("#theme"));
}

// ------------------------------------------------------------------ cart
async function refreshCart() {
  if (!session.user) { state.cart = null; return null; }
  try { state.cart = await api("/cart"); } catch { state.cart = null; }
  return state.cart;
}
async function addToCart(product, qty = 1) {
  if (!session.user) { toast("Sign in to add items to your cart"); return needLogin(); }
  const existing = state.cart && state.cart.items.find((i) => i.product_id === product.id);
  const quantity = Math.min(99, (existing ? existing.quantity : 0) + qty);
  try {
    await api(`/cart/items/${product.id}`, { method: "PUT", body: { quantity } });
    await refreshCart(); renderTopbar();
    toast(`Added ${product.name}`);
  } catch (e) { toast(errorText(e), "bad"); }
}
async function openCart() {
  if (!session.user) return needLogin();
  await refreshCart();
  const dlg = h("dialog", { class: "sheet", "aria-labelledby": "cart-t" });
  const body = h("div", { class: "dlg-body" }), foot = h("div", { class: "dlg-foot" });
  const draw = () => {
    const cart = state.cart || { items: [], subtotal_cents: 0, checkout_ready: false };
    mount(body, cart.items.length ? cart.items.map((it) => cartLine(it, draw)) : h("div", { class: "empty" }, h("h3", {}, "Your cart is empty"), h("p", {}, "Add something from the shelf and it will show up here."), h("button", { class: "btn secondary", onclick: () => dlg.close() }, "Keep shopping")));
    mount(foot, h("div", { class: "grow" }, h("div", { class: "muted" }, "Subtotal"), h("div", { class: "price" }, money(cart.subtotal_cents))),
      h("button", { class: "btn", disabled: !cart.items.length, onclick: () => { dlg.close(); go("/checkout"); } }, "Go to checkout"));
  };
  dlg.append(h("div", { class: "dlg-head row" }, h("h2", { id: "cart-t", class: "grow" }, "Your cart"), h("button", { class: "btn ghost icon", "aria-label": "Close cart", onclick: () => dlg.close() }, "✕")), body, foot);
  dlg.addEventListener("close", () => dlg.remove());
  dlg.addEventListener("click", (e) => { if (e.target === dlg) dlg.close(); });
  document.body.append(dlg); draw(); dlg.showModal();
}
function cartLine(it, redraw) {
  const change = async (q) => {
    try {
      if (q < 1) await api(`/cart/items/${it.product_id}`, { method: "DELETE" });
      else await api(`/cart/items/${it.product_id}`, { method: "PUT", body: { quantity: Math.min(99, q) } });
      await refreshCart(); renderTopbar(); redraw();
    } catch (e) { toast(errorText(e), "bad"); }
  };
  const problem = !it.purchasable ? (it.available === 0 ? "No longer available" : `Only ${it.available} left`) : null;
  return h("div", { class: "cartline" }, plate({ id: it.product_id, name: it.name, category: { slug: "default" } }),
    h("div", { class: "stack" }, h("h4", {}, it.name), problem && badge(problem, "warn"),
      h("div", { class: "qty" }, h("button", { "aria-label": "Decrease quantity", onclick: () => change(it.quantity - 1) }, "−"), h("output", {}, it.quantity), h("button", { "aria-label": "Increase quantity", onclick: () => change(it.quantity + 1) }, "+"))),
    h("div", { class: "price" }, money(it.line_total_cents)));
}

// ------------------------------------------------------------------ catalog
function tagCard(p) {
  const slug = p.category.slug;
  const card = h("article", { class: "tagcard" });
  card.style.setProperty("--hue", hueVar(slug));
  const out = p.available <= 0;
  card.append(plate(p), h("div", { class: "body" },
    h("span", { class: "cat" }, p.category.name),
    h("h3", {}, h("a", { href: `#/p/${p.id}` }, p.name)),
    tally(p.available, 5, hueVar(slug)),
    h("div", { class: "foot" }, h("span", { class: "price" }, money(p.price_cents)),
      h("button", { class: "btn small secondary", disabled: out, "aria-label": `Add ${p.name} to cart`, onclick: () => addToCart(p) }, out ? "Sold out" : "Add"))));
  return card;
}

async function catalogView({ params }) {
  const cat = params.get("c") || "", q = params.get("q") || "", sort = params.get("sort") || "newest";
  if (!state.categories.length) state.categories = (await api("/categories")).categories;
  const qs = (extra) => { const p = new URLSearchParams({ ...(cat && { c: cat }), ...(q && { q }), ...(sort !== "newest" && { sort }), ...extra }); for (const [k, v] of [...p]) if (!v) p.delete(k); const s = p.toString(); return "#/" + (s ? "?" + s : ""); };
  const fetchPage = (after) => api("/products?" + new URLSearchParams({ limit: 24, sort, ...(cat && { category: cat }), ...(q && { q }), ...(after && { after }) }));
  let first;
  try { first = await fetchPage(); } catch (e) { if (e.status === 422 && q) first = { items: [], next_cursor: null, badSearch: e.message }; else throw e; }

  // The most characteristic thing in this shop: the one-of-a-kind item, with its real stock.
  let spot = null;
  if (!cat && !q && sort === "newest") { try { spot = (await api("/products?limit=50")).items.find((p) => p.available === 1); } catch { /* optional */ } }

  const rail = h("aside", { class: "rail", "aria-label": "Filters" },
    h("div", {}, h("h3", {}, "Categories"), h("ul", {},
      h("li", {}, h("a", { href: qs({ c: "" }), "aria-current": !cat }, "All products")),
      state.categories.map((c) => { const li = h("li", {}, h("a", { href: qs({ c: c.slug }), "aria-current": cat === c.slug }, c.name, h("span", { class: "count" }, c.product_count))); li.firstChild.style.setProperty("--hue", hueVar(c.slug)); return li; }))),
    h("div", {}, h("h3", {}, "Sort by"), h("div", { class: "seg", role: "group", "aria-label": "Sort" },
      [["newest", "Newest"], ["price_asc", "Price ↑"], ["price_desc", "Price ↓"]].map(([v, l]) => h("button", { "aria-pressed": sort === v, onclick: () => go(qs({ sort: v }).slice(1)) }, l)))));

  const grid = h("div", { class: "grid" });
  const more = h("div", { class: "row" });
  const fill = (page) => {
    grid.append(...page.items.map(tagCard));
    clear(more);
    if (page.next_cursor) more.append(h("button", { class: "btn secondary", onclick: async (e) => { e.target.disabled = true; try { fill(await fetchPage(page.next_cursor)); } catch (err) { toast(errorText(err), "bad"); e.target.disabled = false; } } }, "Show more"));
  };
  fill(first);
  const heading = q ? `Results for “${q}”` : cat ? (state.categories.find((c) => c.slug === cat) || { name: "Products" }).name : "All products";
  document.title = `${heading} · StockForge`;
  const empty = !first.items.length && h("div", { class: "empty" }, h("h3", {}, first.badSearch ? "That search can't be run" : "Nothing matches"), h("p", {}, first.badSearch || "Try fewer words, or browse a category."), h("a", { class: "btn secondary", href: "#/" }, "Clear search"));
  mount(main,
    spot && h("section", { class: "spotlight", "aria-label": "Last one" }, plate(spot, false),
      h("div", { class: "copy" }, h("span", { class: "muted" }, spot.category.name), h("h1", {}, spot.name),
        tally(spot.available, 5, hueVar(spot.category.slug)),
        h("p", { class: "muted" }, "There is exactly one on the shelf. Whoever completes checkout first gets it; everyone else is told straight away."),
        h("div", { class: "row" }, h("span", { class: "price" }, money(spot.price_cents)), h("a", { class: "btn", href: `#/p/${spot.id}` }, "See this one")))),
    h("div", { class: "catalog" }, rail, h("section", { class: "stack-lg", "aria-labelledby": "ph" }, h("h2", { id: "ph" }, heading), empty || grid, more)));
}

// ------------------------------------------------------------------ product page
async function productView({ args }) {
  const { product: p } = await api(`/products/${args[0]}`).catch((e) => { if (e.status === 404) return { product: null }; throw e; });
  if (!p) return mount(main, h("div", { class: "empty" }, h("h1", {}, "That product isn't available"), h("a", { class: "btn", href: "#/" }, "Back to the shop")));
  document.title = `${p.name} · StockForge`;
  let qty = 1;
  const out = h("output", {}, qty);
  const max = Math.max(1, Math.min(99, p.available));
  const set = (n) => { qty = Math.max(1, Math.min(max, n)); out.textContent = qty; };
  const wrap = h("div", { class: "pdp" }); wrap.style.setProperty("--hue", hueVar(p.category.slug));
  mount(main, h("p", { class: "crumbs" }, h("a", { href: "#/" }, "Shop"), " / ", h("a", { href: `#/?c=${p.category.slug}` }, p.category.name)),
    h("div", { class: "pdp" }, plate(p, true),
      h("div", { class: "info" }, h("h1", {}, p.name), h("div", { class: "price" }, money(p.price_cents)),
        tally(p.available, 5, hueVar(p.category.slug)),
        h("p", {}, p.description),
        p.available > 0 && h("div", { class: "row" }, h("div", { class: "qty" }, h("button", { "aria-label": "Decrease quantity", onclick: () => set(qty - 1) }, "−"), out, h("button", { "aria-label": "Increase quantity", onclick: () => set(qty + 1) }, "+")),
          h("button", { class: "btn", onclick: () => addToCart(p, qty) }, "Add to cart")),
        p.available <= 0 && h("div", { class: "alert info" }, "Out of stock. Check back later; restocks appear here as soon as they're recorded."),
        h("ul", { class: "factlist" }, h("li", {}, `SKU ${p.sku}`), h("li", {}, "Stock is counted again at checkout; a cart does not hold items."), h("li", {}, "Payment on this site is simulated. No card details are collected.")))));
}

// ------------------------------------------------------------------ checkout
function idemKeyFor(cart) {
  const fp = cart.items.map((i) => `${i.product_id}x${i.quantity}`).join(",") + "|" + cart.subtotal_cents;
  try {
    const saved = JSON.parse(sessionStorage.getItem("sf_idem") || "null");
    if (saved && saved.fp === fp) return saved.key;                       // same cart: a retry must reuse the key
    const key = crypto.randomUUID();
    sessionStorage.setItem("sf_idem", JSON.stringify({ fp, key }));
    return key;
  } catch { return crypto.randomUUID(); }
}
const SCENARIOS = [["success", "Payment succeeds", "The normal path."], ["decline", "Card is declined", "The order stays reserved so you can try again."], ["timeout", "Provider times out", "The result is unknown; trying again is safe."], ["flaky", "Declines first, then succeeds", "Shows a retry working."]];
function scenarioPicker(name = "scenario") {
  return h("fieldset", { class: "stack", id: "scen" }, h("legend", { class: "sr-only" }, "Simulated payment outcome"),
    SCENARIOS.map(([v, t, d], i) => h("label", { class: "choice" }, h("input", { type: "radio", name, value: v, checked: i === 0 }), h("span", {}, t), h("small", {}, d))));
}
const pickedScenario = () => ($("input[name=scenario]:checked") || { value: "success" }).value;

async function checkoutView() {
  if (!session.user) return needLogin();
  const cart = await refreshCart();
  renderTopbar();
  if (!cart || !cart.items.length) return mount(main, h("div", { class: "empty" }, h("h1", {}, "Your cart is empty"), h("a", { class: "btn", href: "#/" }, "Browse the shop")));
  document.title = "Checkout · StockForge";
  const msg = h("div", { "aria-live": "polite" });
  const btn = h("button", { class: "btn", disabled: !cart.checkout_ready }, `Place order and pay ${money(cart.subtotal_cents)}`);
  const place = async () => {
    btn.disabled = true; clear(msg);
    try {
      const r = await api("/orders", { method: "POST", body: { expected_total_cents: cart.subtotal_cents }, headers: { "Idempotency-Key": idemKeyFor(cart) } });
      sessionStorage.removeItem("sf_idem");
      state.cart = null; renderTopbar();
      const oid = r.order.id;
      try { await api(`/orders/${oid}/pay`, { method: "POST", body: { scenario: pickedScenario() } }); toast("Order paid. Thank you!"); }
      catch (e) { toast(`Order reserved, but payment didn't go through: ${e.message}`, "bad"); }
      go(`/orders/${oid}`);
    } catch (e) {
      btn.disabled = false;
      if (e.code === "INSUFFICIENT_STOCK") {
        const short = (e.detail.items || []).map((s) => h("li", {}, `${s.sku}: you asked for ${s.requested}, ${s.available} left`));
        mount(msg, h("div", { class: "alert" }, h("strong", {}, "Someone got there first. "), "Nothing was reserved or charged.", h("ul", {}, short), h("a", { href: "#/" }, "Adjust your cart")));
        await refreshCart(); renderTopbar();
      } else if (e.code === "PRICE_CHANGED") {
        mount(msg, h("div", { class: "alert" }, `The total changed to ${money(e.detail.current_total_cents)}. Review your cart and try again.`)); sessionStorage.removeItem("sf_idem");
        await refreshCart(); setTimeout(route, 1500);
      } else mount(msg, h("div", { class: "alert" }, errorText(e), " You can press the button again; it won't create a second order."));
    }
  };
  btn.addEventListener("click", place);
  mount(main, h("h1", {}, "Checkout"), h("div", { class: "checkout" },
    h("section", { class: "stack-lg" },
      h("div", { class: "panel" }, cart.items.map((it) => h("div", { class: "orderrow" }, h("div", {}, h("strong", {}, it.name), h("div", { class: "muted" }, `${it.quantity} × ${money(it.unit_price_cents)}`), !it.purchasable && badge(it.available ? `Only ${it.available} left` : "Not available", "warn")), h("span", {}), h("span", { class: "price" }, money(it.line_total_cents))))),
      h("div", { class: "stack" }, h("h2", {}, "Payment"), h("div", { class: "alert info" }, "This store uses a payment simulator. Choose what should happen; no card details are collected."), scenarioPicker())),
    h("aside", { class: "panel summary" }, h("h2", {}, "Order summary"), h("div", { class: "totals" }, h("span", {}, "Total"), h("span", { class: "price" }, money(cart.subtotal_cents))),
      !cart.checkout_ready && h("div", { class: "alert" }, "Some items in your cart aren't available in these quantities. Fix them in your cart first."), btn, msg,
      h("p", { class: "muted" }, "Pressing the button twice, or retrying after a network error, never creates two orders."))));
}

// ------------------------------------------------------------------ orders
async function ordersView() {
  if (!session.user) return needLogin();
  document.title = "Your orders · StockForge";
  const r = await api("/orders?limit=50");
  mount(main, h("h1", {}, "Your orders"), r.items.length
    ? h("div", { class: "panel" }, r.items.map((o) => h("a", { class: "orderrow", href: `#/orders/${o.id}` }, h("div", {}, h("strong", {}, `Order ${o.id.slice(-8)}`), h("div", { class: "muted" }, `${when(o.created_at)}, ${o.item_count} item${o.item_count === 1 ? "" : "s"}`)), statusBadge(o.status), h("span", { class: "price" }, money(o.total_cents)))))
    : h("div", { class: "empty" }, h("h3", {}, "No orders yet"), h("p", {}, "When you place an order it will show up here, with its status."), h("a", { class: "btn", href: "#/" }, "Start shopping")));
}

function stepper(o) {
  if (o.status === "CANCELLED" || o.status === "EXPIRED") return h("div", { class: "alert" }, o.status === "EXPIRED" ? "This order expired before it was paid, and its items went back on the shelf." : "This order was cancelled and its items went back on the shelf.");
  const steps = [["Placed", true], ["Paid", ["PAID", "SHIPPED", "DELIVERED"].includes(o.status)], ["Shipped", ["SHIPPED", "DELIVERED"].includes(o.status)], ["Delivered", o.status === "DELIVERED"]];
  const lastDone = steps.map((s) => s[1]).lastIndexOf(true);
  return h("ol", { class: "timeline", "aria-label": "Order progress" }, steps.map(([t, done], i) => h("li", { class: (done ? "done" : "") + (i === lastDone ? " now" : ""), "aria-current": i === lastDone ? "step" : null }, t)));
}

async function orderView({ args }) {
  if (!session.user) return needLogin();
  const draw = async () => {
    const { order: o } = await api(`/orders/${args[0]}`);
    document.title = `Order ${o.id.slice(-8)} · StockForge`;
    const pending = o.status === "PENDING_PAYMENT";
    const actions = [];
    if (pending) {
      actions.push(h("div", { class: "panel summary" }, h("h2", {}, "Finish paying"), h("p", { class: "muted" }, o.expires_at ? `These items are held for you until ${when(o.expires_at)}. After that they go back on the shelf.` : ""), scenarioPicker(),
        h("div", { class: "row" }, h("button", { class: "btn", onclick: async (e) => { e.target.disabled = true; try { await api(`/orders/${o.id}/pay`, { method: "POST", body: { scenario: pickedScenario() } }); toast("Payment received"); } catch (err) { toast(errorText(err), "bad"); } draw(); } }, `Pay ${money(o.total_cents)}`),
          h("button", { class: "btn danger", onclick: cancelIt(o, draw) }, "Cancel order"))));
    } else if (o.status === "PAID") actions.push(h("button", { class: "btn danger", onclick: cancelIt(o, draw) }, "Cancel order"));
    mount(main, h("p", { class: "crumbs" }, h("a", { href: "#/orders" }, "Your orders"), ` / ${o.id.slice(-8)}`),
      h("div", { class: "row wrap" }, h("h1", { class: "grow" }, `Order ${o.id.slice(-8)}`), statusBadge(o.status)),
      h("p", { class: "muted" }, `Placed ${when(o.created_at)}`), stepper(o),
      h("div", { class: "checkout" }, h("section", { class: "stack-lg" },
        h("div", { class: "panel" }, o.items.map((it) => h("div", { class: "orderrow" }, h("div", {}, h("strong", {}, it.name), h("div", { class: "muted" }, `${it.quantity} × ${money(it.unit_price_cents)}`)), h("span", {}), h("span", { class: "price" }, money(it.line_total_cents)))),
          h("div", { class: "orderrow" }, h("strong", {}, "Total"), h("span", {}), h("span", { class: "price" }, money(o.total_cents)))),
        o.payments.length > 0 && h("div", { class: "stack" }, h("h2", {}, "Payment attempts"), h("div", { class: "panel" }, o.payments.map((p) => h("div", { class: "orderrow" }, h("span", {}, `Attempt ${p.attempt}`), badge(p.status === "succeeded" ? "Succeeded" : p.status === "failed" ? "Declined" : "Timed out", p.status === "succeeded" ? "ok" : "bad"), h("span", { class: "muted" }, p.scenario)))))),
        h("aside", { class: "stack" }, ...actions)));
  };
  await draw();
}
const cancelIt = (o, redraw) => async () => {
  if (!(await confirmDialog({ title: "Cancel this order?", body: o.status === "PAID" ? "The items go back on the shelf and the payment is reversed (simulated)." : "The items go back on the shelf.", confirm: "Cancel order", danger: true }))) return;
  try { await api(`/orders/${o.id}/cancel`, { method: "POST" }); toast("Order cancelled"); } catch (e) { toast(errorText(e), "bad"); }
  redraw();
};

// ------------------------------------------------------------------ sign in / register
function authView({ params }, mode) {
  const reg = mode === "register";
  document.title = `${reg ? "Create account" : "Sign in"} · StockForge`;
  const next = params.get("next") || "/";
  const err = h("div", { "aria-live": "polite" });
  // explicit id/for pairs: do not rely on implicit label association for the accessible name
  const fields = {
    name: h("input", { id: "f-name", class: "input", name: "display_name", autocomplete: "name", required: true }),
    email: h("input", { id: "f-email", class: "input", name: "email", type: "email", autocomplete: "email", required: true, autofocus: !reg }),
    pw: h("input", { id: "f-pw", class: "input", name: "password", type: "password", autocomplete: reg ? "new-password" : "current-password", required: true, minlength: reg ? 10 : null }),
  };
  const fieldErr = { display_name: h("span", { class: "field-error" }), email: h("span", { class: "field-error" }), password: h("span", { class: "field-error" }) };
  const submit = h("button", { class: "btn", type: "submit" }, reg ? "Create account" : "Sign in");
  const form = h("form", { novalidate: true, onsubmit: async (e) => {
    e.preventDefault(); submit.disabled = true; clear(err); Object.values(fieldErr).forEach(clear);
    try {
      if (reg) { await api("/auth/register", { method: "POST", body: { email: fields.email.value, password: fields.pw.value, display_name: fields.name.value } }); }
      await login(fields.email.value, fields.pw.value);
      await refreshCart(); toast(reg ? "Welcome to StockForge" : "Signed in"); go(decodeURIComponent(next));
    } catch (ex) {
      submit.disabled = false;
      if (ex instanceof ApiError && ex.field && fieldErr[ex.field]) fieldErr[ex.field].textContent = ex.message;
      else mount(err, h("div", { class: "alert" }, ex.code === "RATE_LIMITED" ? "Too many attempts. Wait a moment and try again." : errorText(ex)));
    }
  } },
    reg && h("div", { class: "field" }, h("label", { for: "f-name" }, "Your name"), fields.name, fieldErr.display_name),
    h("div", { class: "field" }, h("label", { for: "f-email" }, "Email"), fields.email, fieldErr.email),
    h("div", { class: "field" }, h("label", { for: "f-pw" }, "Password"), reg && h("small", { class: "muted" }, "At least 10 characters"), fields.pw, fieldErr.password), submit, err);
  const demo = ["localhost", "127.0.0.1"].includes(location.hostname) && !reg && h("details", { class: "panel authdemo" }, h("summary", {}, "Demo accounts (local development only)"),
    h("p", { class: "muted" }, "Customer: ava@stockforge.test with Customer-Demo-2026!"), h("p", { class: "muted" }, "Admin: admin@stockforge.test with Admin-Demo-2026!"));
  mount(main, h("div", { class: "authbox" }, h("h1", {}, reg ? "Create your account" : "Sign in"), form,
    h("p", { class: "muted" }, reg ? "Already have an account? " : "New here? ", h("a", { href: reg ? `#/login?next=${next}` : `#/register?next=${next}` }, reg ? "Sign in" : "Create an account")), demo));
}

// ------------------------------------------------------------------ boot
window.addEventListener("hashchange", route);
(async () => { await loadSession(); await refreshCart(); route(); })();
