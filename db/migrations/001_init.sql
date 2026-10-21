-- StockForge schema v1. MySQL 8.4 / InnoDB / utf8mb4.
-- Money is integer minor units (cents). Stock quantities are never negative:
-- enforced by UNSIGNED columns AND CHECK constraints (defence in depth).

CREATE TABLE IF NOT EXISTS schema_migrations (
  version     VARCHAR(32) NOT NULL PRIMARY KEY,
  applied_at  TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
) ENGINE=InnoDB;

-- ---------------------------------------------------------------- identity
CREATE TABLE users (
  id             BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
  email          VARCHAR(254) NOT NULL,
  password_hash  VARCHAR(255) NOT NULL,              -- argon2id (libsodium) encoded string
  display_name   VARCHAR(100) NOT NULL,
  role           ENUM('customer','admin') NOT NULL DEFAULT 'customer',
  is_active      TINYINT(1) NOT NULL DEFAULT 1,
  created_at     TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
  UNIQUE KEY uq_users_email (email)
) ENGINE=InnoDB;

-- Only a SHA-256 of the session token is stored, so a DB leak cannot be replayed.
CREATE TABLE sessions (
  token_hash   BINARY(32) NOT NULL PRIMARY KEY,
  user_id      BIGINT UNSIGNED NOT NULL,
  csrf_token   CHAR(43) NOT NULL,                    -- base64url(32 bytes)
  created_at   TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
  expires_at   TIMESTAMP(3) NOT NULL,
  CONSTRAINT fk_sessions_user FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,
  KEY ix_sessions_user (user_id),
  KEY ix_sessions_expiry (expires_at)
) ENGINE=InnoDB;

-- ----------------------------------------------------------------- catalog
CREATE TABLE categories (
  id    INT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
  slug  VARCHAR(80)  NOT NULL,
  name  VARCHAR(120) NOT NULL,
  UNIQUE KEY uq_categories_slug (slug)
) ENGINE=InnoDB;

CREATE TABLE products (
  id            BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
  sku           VARCHAR(40)  NOT NULL,
  category_id   INT UNSIGNED NOT NULL,
  name          VARCHAR(200) NOT NULL,
  description   TEXT NOT NULL,
  price_cents   INT UNSIGNED NOT NULL,
  currency      CHAR(3) NOT NULL DEFAULT 'USD',
  image_url     VARCHAR(500) NULL,
  is_active     TINYINT(1) NOT NULL DEFAULT 1,
  created_at    TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
  updated_at    TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),
  UNIQUE KEY uq_products_sku (sku),
  CONSTRAINT fk_products_category FOREIGN KEY (category_id) REFERENCES categories(id),
  CONSTRAINT ck_products_price CHECK (price_cents > 0),
  -- list page: filter by category, sort by price / newest, keyset-paginate on id
  KEY ix_products_cat_price (category_id, is_active, price_cents, id),
  KEY ix_products_active_new (is_active, created_at, id),
  FULLTEXT KEY ft_products_search (name, description)
) ENGINE=InnoDB;

-- available = on_hand - reserved. `reserved` is a denormalised SUM of held
-- reservations, maintained in the same transaction and verified by reconcile.
CREATE TABLE inventory (
  product_id           BIGINT UNSIGNED NOT NULL PRIMARY KEY,
  on_hand              INT UNSIGNED NOT NULL,
  reserved             INT UNSIGNED NOT NULL DEFAULT 0,
  low_stock_threshold  INT UNSIGNED NOT NULL DEFAULT 5,
  updated_at           TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),
  CONSTRAINT fk_inventory_product FOREIGN KEY (product_id) REFERENCES products(id),
  CONSTRAINT ck_inventory_reserved_le_onhand CHECK (reserved <= on_hand)
) ENGINE=InnoDB;

CREATE TABLE cart_items (
  user_id     BIGINT UNSIGNED NOT NULL,
  product_id  BIGINT UNSIGNED NOT NULL,
  quantity    INT UNSIGNED NOT NULL,
  updated_at  TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),
  PRIMARY KEY (user_id, product_id),
  CONSTRAINT fk_cart_user    FOREIGN KEY (user_id)    REFERENCES users(id) ON DELETE CASCADE,
  CONSTRAINT fk_cart_product FOREIGN KEY (product_id) REFERENCES products(id),
  CONSTRAINT ck_cart_qty CHECK (quantity BETWEEN 1 AND 99)
) ENGINE=InnoDB;

-- ------------------------------------------------------------------ orders
-- Legal transitions are documented in docs/ARCHITECTURE.md and enforced in
-- src/order_state.c (single table-driven function). `version` supports
-- compare-and-set updates so two racing transitions cannot both win.
CREATE TABLE orders (
  id           BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
  public_id    CHAR(26) NOT NULL,                    -- ULID-style, safe to expose
  user_id      BIGINT UNSIGNED NOT NULL,
  status       ENUM('PENDING_PAYMENT','PAID','SHIPPED','DELIVERED','CANCELLED','EXPIRED') NOT NULL,
  total_cents  BIGINT UNSIGNED NOT NULL,
  currency     CHAR(3) NOT NULL DEFAULT 'USD',
  version      INT UNSIGNED NOT NULL DEFAULT 1,
  expires_at   TIMESTAMP(3) NULL,                    -- reservation deadline while PENDING_PAYMENT
  created_at   TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
  updated_at   TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),
  UNIQUE KEY uq_orders_public (public_id),
  CONSTRAINT fk_orders_user FOREIGN KEY (user_id) REFERENCES users(id),
  KEY ix_orders_user (user_id, id),
  KEY ix_orders_status_created (status, created_at, id),
  KEY ix_orders_pending_expiry (status, expires_at)   -- expiry sweeper
) ENGINE=InnoDB;

CREATE TABLE order_items (
  id                BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
  order_id          BIGINT UNSIGNED NOT NULL,
  product_id        BIGINT UNSIGNED NOT NULL,
  sku               VARCHAR(40)  NOT NULL,           -- snapshots: history survives catalog edits
  product_name      VARCHAR(200) NOT NULL,
  unit_price_cents  INT UNSIGNED NOT NULL,
  quantity          INT UNSIGNED NOT NULL,
  UNIQUE KEY uq_order_items (order_id, product_id),
  CONSTRAINT fk_oi_order   FOREIGN KEY (order_id)   REFERENCES orders(id) ON DELETE CASCADE,
  CONSTRAINT fk_oi_product FOREIGN KEY (product_id) REFERENCES products(id),
  CONSTRAINT ck_oi_qty CHECK (quantity BETWEEN 1 AND 99)
) ENGINE=InnoDB;

-- One reservation per (order, product). held -> committed (paid) | released (cancelled/expired).
CREATE TABLE stock_reservations (
  id          BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
  order_id    BIGINT UNSIGNED NOT NULL,
  product_id  BIGINT UNSIGNED NOT NULL,
  quantity    INT UNSIGNED NOT NULL,
  status      ENUM('held','committed','released') NOT NULL DEFAULT 'held',
  created_at  TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
  updated_at  TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),
  UNIQUE KEY uq_res_order_product (order_id, product_id),
  CONSTRAINT fk_res_order   FOREIGN KEY (order_id)   REFERENCES orders(id) ON DELETE CASCADE,
  CONSTRAINT fk_res_product FOREIGN KEY (product_id) REFERENCES products(id),
  CONSTRAINT ck_res_qty CHECK (quantity > 0),
  KEY ix_res_product_status (product_id, status)
) ENGINE=InnoDB;

-- Simulated provider only. No card data is ever stored (see docs/SECURITY.md).
CREATE TABLE payment_attempts (
  id           BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
  order_id     BIGINT UNSIGNED NOT NULL,
  attempt_no   INT UNSIGNED NOT NULL,
  scenario     ENUM('success','decline','timeout','flaky') NOT NULL,
  status       ENUM('pending','succeeded','failed','timed_out') NOT NULL,
  amount_cents BIGINT UNSIGNED NOT NULL,
  provider_ref CHAR(26) NOT NULL,
  created_at   TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
  UNIQUE KEY uq_pay_order_attempt (order_id, attempt_no),
  UNIQUE KEY uq_pay_provider_ref (provider_ref),
  CONSTRAINT fk_pay_order FOREIGN KEY (order_id) REFERENCES orders(id) ON DELETE CASCADE
) ENGINE=InnoDB;

-- --------------------------------------------------------------- idempotency
-- Row is inserted INSIDE the checkout transaction, so "key exists" and "order
-- exists" commit atomically. UNIQUE(user_id, idem_key) serialises concurrent
-- duplicates at the database, across any number of server processes.
CREATE TABLE idempotency_keys (
  id              BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
  user_id         BIGINT UNSIGNED NOT NULL,
  idem_key        VARCHAR(64) NOT NULL,
  request_hash    BINARY(32) NOT NULL,               -- same key + different body => 422
  order_id        BIGINT UNSIGNED NULL,
  response_status SMALLINT UNSIGNED NULL,
  response_body   JSON NULL,
  created_at      TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
  UNIQUE KEY uq_idem (user_id, idem_key),
  CONSTRAINT fk_idem_user  FOREIGN KEY (user_id)  REFERENCES users(id) ON DELETE CASCADE,
  CONSTRAINT fk_idem_order FOREIGN KEY (order_id) REFERENCES orders(id),
  KEY ix_idem_created (created_at)
) ENGINE=InnoDB;

-- ---------------------------------------------------- ledger + audit trail
-- Append-only. Reconciliation: for every product,
--   on_hand  == seed_on_hand + SUM(delta_on_hand)
--   reserved == SUM(delta_reserved) == SUM(quantity) of held reservations.
CREATE TABLE inventory_ledger (
  id              BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
  product_id      BIGINT UNSIGNED NOT NULL,
  delta_on_hand   INT NOT NULL,
  delta_reserved  INT NOT NULL,
  reason          ENUM('seed','restock','adjust','reserve','commit','release') NOT NULL,
  order_id        BIGINT UNSIGNED NULL,
  created_at      TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
  CONSTRAINT fk_ledger_product FOREIGN KEY (product_id) REFERENCES products(id),
  KEY ix_ledger_product (product_id, id),
  KEY ix_ledger_order (order_id)
) ENGINE=InnoDB;

CREATE TABLE audit_events (
  id             BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
  request_id     CHAR(16) NULL,
  actor_user_id  BIGINT UNSIGNED NULL,
  action         VARCHAR(60) NOT NULL,               -- e.g. order.created, order.cancelled
  entity_type    VARCHAR(40) NOT NULL,
  entity_id      VARCHAR(40) NOT NULL,
  details        JSON NULL,                          -- never contains secrets or PII beyond ids
  created_at     TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
  KEY ix_audit_entity (entity_type, entity_id, id),
  KEY ix_audit_actor (actor_user_id, id),
  KEY ix_audit_created (created_at, id)
) ENGINE=InnoDB;

INSERT INTO schema_migrations (version) VALUES ('001');
