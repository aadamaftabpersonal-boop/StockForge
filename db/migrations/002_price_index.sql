-- Price-sorted catalog browsing without a category filter:
--   WHERE is_active = 1 ORDER BY price_cents [DESC], id [DESC] LIMIT n  (keyset on (price_cents, id))
-- Without this index MySQL must filesort every active product. The existing
-- ix_products_cat_price already serves the category-filtered variant.
-- (Verified with EXPLAIN in docs/DB_QUERY_PLANS.md once phase 6 benchmarks run.)
CREATE INDEX ix_products_active_price ON products (is_active, price_cents, id);

INSERT INTO schema_migrations (version) VALUES ('002');
