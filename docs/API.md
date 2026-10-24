# HTTP API

Base path `/api/v1`. All bodies are JSON (`Content-Type: application/json`). Every
response carries a `request_id` in errors and an `X-Request-Id` header, and the same id
appears in the server log, so one failing call can be traced end to end.

The examples below are **real output** captured from the running Compose stack
(`curl` against Apache on `http://localhost:8081`, dev-HTTP mode, demo seed). Ids and
timestamps differ on every run.

## Conventions

| | |
|---|---|
| Authentication | Session cookie set by `POST /auth/login`. The cookie is `HttpOnly`, `SameSite=Lax`, and (behind TLS) `Secure`. The server stores only a SHA-256 hash of the token. |
| CSRF | Every non-GET call on a session route needs `X-CSRF-Token` (returned by `login` and `auth/me`). Missing or wrong: `403 CSRF_INVALID`. |
| Origin | If a request carries `Origin`, it must match `Host`: else `403 ORIGIN_MISMATCH`. |
| Content type | A request with a body must be `application/json`: else `415`. |
| Money | Integer cents plus `currency` (`USD`). Never floating point. |
| Errors | `{"error":{"code","message","request_id", "field"?}}`. `field` names the offending input for `422`. |
| Paging | `limit` (default 20; max 50 on the public catalogue, 100 on admin lists) and the opaque `next_cursor` from the previous page, passed as `after`. `next_cursor` is `null` on the last page. |
| Rate limits | Per client IP and per account on login/register, per user on checkout. Over the limit: `429 RATE_LIMITED` with `Retry-After`. |

### Status codes used

`200` ok, `201` created, `401 UNAUTHENTICATED`, `402 PAYMENT_DECLINED` / `PAYMENT_TIMEOUT`,
`403 FORBIDDEN`, `404 NOT_FOUND`, `409` (a conflict: `INSUFFICIENT_STOCK`, `IDEMPOTENCY_KEY_REUSED`,
`ILLEGAL_TRANSITION`, `SLUG_TAKEN`, `SKU_TAKEN`, `BELOW_RESERVED`, `SELF_MODIFY`),
`415`, `422 VALIDATION_FAILED`, `429`, `503` (queue full, or database unavailable).

## Authentication

### `POST /auth/login`

```bash
curl -c jar -H 'Content-Type: application/json' \
  -d '{"email":"ben@stockforge.test","password":"<demo password>"}' http://localhost:8081/api/v1/auth/login
```
```json
{"user":{"id":3,"email":"ben@stockforge.test","display_name":"Ben Okafor","role":"customer"},
 "csrf_token":"FVUdt8Zda4QNtigKrfp1mX29tn2kGptJOW0IvJ4pb9E"}
```

Wrong email and wrong password are indistinguishable by design:

```json
{"error":{"code":"INVALID_CREDENTIALS","message":"Invalid email or password.","request_id":"98f398c9c9f7e4d7"}}
```

Also: `POST /auth/register`, `POST /auth/logout`, `GET /auth/me`.

### Errors you will see on any protected route without a session

```json
{"error":{"code":"UNAUTHENTICATED","message":"Sign in required.","request_id":"2b159b817784aeb6"}}
```

## Catalogue (public)

`GET /categories`, `GET /products`, `GET /products/{id}`.
`GET /products` accepts `category` (slug), `q` (words of 3 or more letters; must be valid UTF-8),
`limit`, `after`.

```bash
curl 'http://localhost:8081/api/v1/products?limit=2&category=audio'
```
```json
{"items":[
  {"id":8,"sku":"SF-AU-008","name":"Coil Braided Headphone Cable","price_cents":1900,"currency":"USD",
   "image_url":null,"category":{"id":1,"slug":"audio","name":"Audio"},
   "available":0,"stock_status":"out_of_stock", "description":"..."}],
 "next_cursor":"8"}
```

`available` is `on_hand - reserved`: units a new order could still claim.
`stock_status` is `in_stock`, `low_stock` or `out_of_stock`.

## Cart (signed in)

`GET /cart`, `PUT /cart/items/{product_id}` with `{"quantity": n}`, `DELETE /cart/items/{product_id}`,
`DELETE /cart`. A cart holds **no stock**; it is only a list of intentions.

```bash
curl -b jar -X PUT -H 'Content-Type: application/json' -H "X-CSRF-Token: $T" \
  -d '{"quantity":2}' http://localhost:8081/api/v1/cart/items/48
```
```json
{"product_id":48,"quantity":2}
```
```json
{"items":[{"product_id":48,"sku":"SF-PW-008","name":"Reusable Cable Ties (50)","unit_price_cents":800,
           "quantity":2,"line_total_cents":1600,"available":12,"purchasable":true}],
 "subtotal_cents":1600,"currency":"USD","checkout_ready":true}
```

## Checkout (the core of the system)

### `POST /orders`: place an order from the cart

Requires `Idempotency-Key: <8-64 characters of A-Z a-z 0-9 - _>` (missing: `400 IDEMPOTENCY_KEY_REQUIRED`; malformed: `400 IDEMPOTENCY_KEY_INVALID`). The same key with the same cart returns the
**same order** (`Idempotent-Replay: true`); the same key with a different cart is
`409 IDEMPOTENCY_KEY_REUSED`. Retrying a request whose answer you never received is therefore
always safe. Design and proofs: [CHECKOUT.md](CHECKOUT.md).

```bash
curl -b jar -X POST -H 'Content-Type: application/json' -H "X-CSRF-Token: $T" \
  -H 'Idempotency-Key: demo-1234-key' -d '{}' http://localhost:8081/api/v1/orders
```
```
HTTP/1.1 201 Created
Idempotent-Replay: false
```
```json
{"order":{"id":"01M5QFQRM97YR7JWK0CZE4MN6S","status":"PENDING_PAYMENT","total_cents":1600,"currency":"USD",
          "created_at":"2026-10-24T17:36:03Z","expires_at":"2026-10-24T17:51:03Z",
          "items":[{"product_id":48,"sku":"SF-PW-008","name":"Reusable Cable Ties (50)","unit_price_cents":800,
                    "quantity":2,"line_total_cents":1600}],
          "payments":[]}}
```

Sending the identical request again returns `201` with `Idempotent-Replay: true` and the same body.
The stock is reserved until `expires_at` (15 minutes by default); the expiry sweeper then
releases it.

Not enough stock (nothing is reserved, nothing is partially applied):

```json
{"error":{"code":"INSUFFICIENT_STOCK","message":"Not enough stock for one or more items.",
          "request_id":"11feddc768c13796",
          "items":[{"sku":"SF-AU-008","requested":2,"available":0}]}}
```

### `POST /orders/{id}/pay`

Body `{"scenario": "success" | "decline" | "timeout" | "flaky"}` selects the simulated payment
provider's behaviour (the provider is a stand-in; see [CHECKOUT.md](CHECKOUT.md)). A decline
leaves the order payable and the stock reserved:

```
HTTP/1.1 402
{"error":{"code":"PAYMENT_DECLINED","message":"The payment was declined. You can try again.","request_id":"9997e44dc197aae2"}}
```

Then success (note both attempts are recorded; a paid order has exactly one successful payment):

```
HTTP/1.1 200
```
```json
{"order":{"id":"01M5QFQRM97YR7JWK0CZE4MN6S","status":"PAID","total_cents":1600,"expires_at":null,
          "payments":[{"attempt":1,"status":"failed","scenario":"decline","amount_cents":1600},
                      {"attempt":2,"status":"succeeded","scenario":"success","amount_cents":1600}], "...":"..."}}
```

Paying an already-paid order is a no-op that returns the order again, never a second charge.

### `GET /orders`, `GET /orders/{id}`, `POST /orders/{id}/cancel`

Customers only ever see their own orders; another customer's order id answers `404`,
indistinguishable from an id that does not exist.

## Order lifecycle

```
PENDING_PAYMENT --pay--> PAID --ship--> SHIPPED --deliver--> DELIVERED
      |                    |
      +--cancel / expire   +--cancel (admin or customer, before shipping)
      v                    v
  CANCELLED / EXPIRED   CANCELLED
```

The table of legal transitions lives in `src/order_state.c` and is also read by the tests.

## Admin (role `admin`; every route below answers 401 anonymously, 403 to customers)

| Route | Purpose |
|---|---|
| `GET /admin/stats` | revenue (total, 30 d, previous 30 d), orders by status, 45-day daily series, inventory and customer counts, top products |
| `GET /admin/orders` | filters `status`, `q` (order number or customer email), keyset paging |
| `GET /admin/orders/{id}` | detail with customer, items, payments, audit history |
| `POST /admin/orders/{id}/transition` | body `{"to":"SHIPPED"\|"DELIVERED"\|"CANCELLED"}`; illegal moves are `409 ILLEGAL_TRANSITION`; repeating a move that already happened is a no-op `200` |
| `GET /admin/products` | includes hidden products; filters `q`, `category`, `stock=low\|out` |
| `POST /admin/products` | create (`sku`, `name`, `description`, `price_cents`, `category_id`, `initial_stock`) |
| `PATCH /admin/products/{id}` | any subset of `name`, `description`, `price_cents`, `category_id`, `is_active`, `image_url`, `low_stock_threshold` |
| `POST /admin/inventory/{id}/adjust` | `{"delta": n, "reason": "restock"\|"adjust"}`; cannot take stock below what open orders reserved (`409 BELOW_RESERVED`) |
| `POST /admin/categories`, `PATCH /admin/categories/{id}` | create / rename |
| `GET /admin/users`, `PATCH /admin/users/{id}` | list with order counts and spend; `{"is_active": false}` disables and signs the user out everywhere |
| `GET /admin/audit` | filter `action`; append-only record of sign-ins, order changes and admin actions |
| `GET /admin/reconcile` | runs every consistency invariant against the live database |
| `GET /admin/metrics` | the same figures as `/metrics`, for the console's System page |

Duplicate SKU, as the console receives it (`422`/`409` carry the `field` so the form can mark the input):

```json
{"error":{"code":"SKU_TAKEN","message":"A product with this SKU already exists.","field":"sku","request_id":"..."}}
```

## Operational endpoints

* `GET /healthz`: liveness plus a database ping (`{"status":"ok","database":"up","version":"0.4.0","uptime_s":310,"workers":8,"queue_depth":0,"queue_capacity":64}`; `503` when the database is down). Exposed through Apache.
* `GET /metrics`: Prometheus text. **Not** exposed through Apache (it answers 403 there); reach it on the app port from inside the network, with `SF_METRICS_TOKEN` as a bearer token, or as an admin via `/api/v1/admin/metrics`.
