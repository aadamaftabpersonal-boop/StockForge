# Local changes to vendored cJSON (upstream v1.7.18, MIT)

1. `cJSON.c`: `global_error` is now `static _Thread_local`.
   Upstream keeps one process-wide error struct that every `cJSON_Parse*` call
   writes, which is a data race when several worker threads parse request
   bodies at once. ThreadSanitizer reported it during the concurrent
   stock-adjustment test (`test_admin.StockAdjust`). We never use
   `cJSON_GetErrorPtr`, so the only effect is that the race is gone.

When upgrading cJSON, re-apply this change and re-run
`SAN=thread` (see README) to confirm no new races.
