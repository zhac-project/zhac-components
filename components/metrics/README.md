# metrics — zero-allocation metrics engine (P4 + S3)

Static-storage, lock-free metrics core shared by both chips. One X-macro
registry drives the `MetricId` enum, the descriptor table, and the
kind-local index mapping at compile time. Storage is a fixed array of
per-shard atomic structs; readers walk all shards and merge. No
heap, no `std::string`, no `std::vector`, no exceptions.

Phased delivery (P0–P5) is shipped through P3. See
`docs/plans/2026-04-21-metrics-engine-plan.md` for the original plan.

## Purpose

Single source of truth for runtime instrumentation: HAP frame counts,
ZCL decode timings, ZNP call latencies, heap watermarks, Lua VM stats,
adapter cache hit ratios. Both chips record into the same X-macro
registry; the S3 exposes the merged view via `/metrics` (Prometheus
text) and an MQTT JSON snapshot publisher.

## Where it sits

- **Chips:** both. Identical registry on P4 and S3 to keep build output
  uniform; P4-only metrics are compiled in on S3 and emit zero-sample
  gauges so a single grep across `metric_registry.def` is the catalogue.
- **Called by — initialisation:**
  - `zhac-main-core/main/main.cpp:132` — `metrics::init()`
  - `zhac-net-core/main/main.cpp:483` — `metrics::init()`
- **Called by — recording (representative):**
  - `zhac-net-core/main/metrics_mqtt.cpp:33` —
    `metrics::update_memory_snapshot()` before each MQTT publish
  - `zhac-net-core/main/rest_ops.cpp:95` —
    `metrics::update_memory_snapshot()` before serving `/metrics`
  - `zhac-main-core/main/hap_dispatch.cpp:1033` — heap snapshot
    refresh prior to building the cross-chip METRICS_RSP
  - direct `_METRIC_*` macro use is scattered across HAP RX, ZHC
    decode, ZNP call sites, etc.
- **Called by — exporters:**
  - `zhac-net-core/main/rest_ops.cpp:98` —
    `metrics::prometheus_format(buf+pos, …)` chunked into the HTTP
    response.
  - `zhac-net-core/main/metrics_mqtt.cpp:34` —
    `metrics::mqtt_format_snapshot_json(buf, sizeof buf)` for the
    periodic MQTT publish.
  - `zhac-main-core/main/hap_dispatch.cpp:1036` —
    `metrics::prometheus_format(…)` packaged as METRICS_RSP and
    returned to the S3 for cross-chip merge.
- **Dependencies (`REQUIRES`):** `freertos`, `esp_timer`. Heap
  metrics use `esp_heap_caps`/`esp_system` when
  `CONFIG_METRICS_ENABLE_MEMORY_METRICS=y` (default y).

## Public API

All declarations are in `include/metrics/`. Every function is
`noexcept` and links to a no-op stub when `CONFIG_METRICS_ENABLED=n`.

| Symbol | Purpose | Notes |
|--------|---------|-------|
| `void metrics::init();` | Zero-initialise shard storage. Idempotent; call once at boot. |
| `void counter_inc(MetricId, uint64_t delta = 1);` | Atomic add to the current shard's counter. | `memory_order_relaxed`. |
| `void counter_set(MetricId, uint64_t value);` | Atomic store; converts the metric to gauge-style semantics. Subsequent reads return the externally-set value exactly. |
| `void value_record(MetricId, int64_t sample);` | Sample a signed value (RSSI, queue depth, payload size, …). Updates count/sum/min/max/last on the current shard. |
| `TimerToken timer_start(MetricId);` / `void timer_stop(TimerToken&);` | Free-function timer pair when an RAII scope can't fit. |
| `void timer_record(MetricId, uint32_t duration_us);` | Record a pre-measured duration. |
| `class TimerScope { explicit TimerScope(MetricId) noexcept; ~TimerScope() noexcept; … };` | Preferred RAII timer entry point. Non-copyable. |
| `bool read_timer(MetricId, TimerSnapshot&);` | Walk all shards, merge into `TimerSnapshot { count, sum_us, min_us, max_us, avg_us, last_us, has_data }`. Returns false if `id` is the wrong kind. |
| `bool read_counter(MetricId, CounterSnapshot&);` | Sums every shard into `CounterSnapshot { value }`. |
| `bool read_value(MetricId, ValueSnapshot&);` | Merges into `ValueSnapshot { count, sum, min, max, avg, last, has_data }`. |
| `void update_memory_snapshot();` | Populate `METRIC_HEAP_*` from `esp_heap_caps` + `esp_system`. No-op if memory metrics disabled. |
| `size_t dump_text(char* buf, size_t buf_size);` | Human-readable formatted dump (declared in `metrics.h`). Always NUL-terminates. Returns 0 if `CONFIG_METRICS_ENABLE_TEXT_DUMP=n`. |
| `size_t prometheus_format(char* buf, size_t buf_size, const char* prefix = nullptr);` | Prometheus text exposition. `prefix` defaults to `CONFIG_METRICS_EXPORT_PREFIX` (`"zhac"`). Truncates at `buf_size - 1` — no chunking inside the formatter. |
| `size_t mqtt_format_snapshot_json(char* buf, size_t buf_size);` | One-document JSON snapshot: `{"timers":{…},"counters":{…},"values":{…}}`. Manual `snprintf`, no cJSON, no heap. |

### Macros (`include/metrics/metrics_macros.h`)

```c
_METRIC_TIMER_SCOPE(METRIC_HAP_RX_HANDLE);
_METRIC_START_TIMER(token, METRIC_X);
_METRIC_STOP_TIMER(token);
_METRIC_COUNTER_INC(METRIC_X, delta);
_METRIC_COUNTER_SET(METRIC_X, value);
_METRIC_VALUE(METRIC_X, sample);
_METRIC_UPDATE_MEMORY_SNAPSHOT();
```

When `CONFIG_METRICS_ENABLED=n` every macro expands to
`do { (void)…; } while (0)` (or a trivial `int var = 0;` for the
start-timer pair) — call sites compile to nothing.

## Important constants / sizes

| Symbol | Value | Notes |
|--------|-------|-------|
| Registry size | **21 entries** at HEAD of `metric_registry.def` | Heap (4–5), HAP timer/counter, ZHC/ZNP/shadow timers, Zigbee/adapter counters, Lua heap/coroutine values. |
| `CONFIG_METRICS_NUM_SHARDS` | default `2`, range `1..4` | One atomic shard per slot; matches each chip's core count to avoid cross-core CAS contention. |
| `CONFIG_METRICS_EXPORT_PREFIX` | default `"zhac"` | Prometheus name prefix (`zhac_hap_rx_handle_count`, etc.). |
| `CONFIG_METRICS_MQTT_INTERVAL_S` | default `60`, range `10..3600` | Publisher cadence (consumer-owned in `zhac-net-core/main/metrics_mqtt.cpp`). |
| `MetricId::_COUNT` | sentinel | `static_assert` in `metrics.cpp` keeps it in lock-step with the descriptor table. |
| `kTimerCount` / `kCounterCount` / `kValueCount` | constexpr count per kind | Computed at compile time from the descriptor table; drives storage sizing. |

### Sharding strategy

Each metric kind has its own POD shard struct (`TimerShard`,
`CounterShard`, `ValueShard`); storage is `Sharded<T, kShards>` arrays
sized by `kTimerCount` / `kCounterCount` / `kValueCount`. The current
shard for a record is selected from the FreeRTOS task's core ID, so a
hot path on core 0 never CAS-contends with one on core 1. Readers walk
all shards and merge.

`atomic_min` / `atomic_max` on shard fields are CAS loops (see
`src/metrics_atomic.h`), which keep the structs lock-free without
needing `std::mutex` on readers.

## Wire format / on-disk layout

### X-macro registry

`include/metrics/metric_registry.def` is the **single source of
truth**. The file is included twice with different macro expansions:

```c
// drives MetricId enum
#define METRIC_TIMER(id, name, help)   id,
#define METRIC_COUNTER(id, name, help) id,
#define METRIC_VALUE(id, name, help)   id,
#include "metrics/metric_registry.def"
#undef METRIC_TIMER
#undef METRIC_COUNTER
#undef METRIC_VALUE
```

A second pass in `metrics.cpp` builds `kDescriptors[]` from the same
file. A `static_assert` keeps the enum and table in sync.

Adding a metric is one line in the registry plus a rebuild. The
`#if CONFIG_SPIRAM` guard around `METRIC_HEAP_SPIRAM_FREE_BYTES` is a
template for chip-specific entries — but the project intentionally
prefers always-defined entries (see the comment block in the registry)
to avoid preprocessor-cache fragility on incremental builds. P4-origin
metrics keep a `zhac_p4_*`-style prefix on naming, not on compilation.

### Prometheus text format

Each metric expands to canonical Prometheus exposition text:

```text
# HELP zhac_hap_rx_handle HAP frame RX handle duration
# TYPE zhac_hap_rx_handle summary
zhac_hap_rx_handle_count <count>
zhac_hap_rx_handle_sum <sum_us>
zhac_hap_rx_handle{quantile="0"}   <min_us>
zhac_hap_rx_handle{quantile="1"}   <max_us>
zhac_hap_rx_handle_last <last_us>
# HELP zhac_hap_rx_frames_total HAP frames received
# TYPE zhac_hap_rx_frames_total counter
zhac_hap_rx_frames_total <value>
```

(Names depend on the prefix passed in or `CONFIG_METRICS_EXPORT_PREFIX`.)

### MQTT JSON snapshot

```json
{
  "timers":   { "hap_rx_handle":  { "count":…, "sum_us":…, "min_us":…, "max_us":…, "avg_us":…, "last_us":… }, … },
  "counters": { "hap_rx_frames_total": …, … },
  "values":   { "heap_free_bytes": { "count":…, "sum":…, "min":…, "max":…, "avg":…, "last":… }, … }
}
```

`mqtt_format_snapshot_json` always NUL-terminates and returns the byte
count excluding the NUL (or 0 on zero-size buffer / when the exporter
is disabled).

## Threading & concurrency

- All shard reads/writes use `std::atomic<…>` with
  `memory_order_relaxed`. No barriers, no fences. Snapshot reads can
  race with concurrent writes — small inconsistencies (e.g. `count`
  observed slightly ahead of `sum_us`) are accepted as a deliberate
  design choice (see `## Constraints`).
- Min/max are CAS loops (`atomic_min` / `atomic_max` in
  `metrics_atomic.h`).
- Counter storage is a single `std::atomic<uint64_t>` per shard; both
  `counter_inc` (fetch-add) and `counter_set` (store) are lock-free on
  the supported targets.
- The shard index is derived from the calling FreeRTOS task's core ID,
  so steady-state recording from a pinned task is uncontended.
- Readers walk every shard; this is not lock-free with respect to
  individual shard updates, but it never blocks a writer.
- Exporter functions are pure formatters — they never mutate state.

## Error / failure modes

| Condition | Behaviour |
|-----------|-----------|
| Wrong-kind `read_*` (e.g. `read_counter` on a Timer id) | Returns `false`; `out` untouched. |
| Truncated buffer in `prometheus_format` / `mqtt_format_snapshot_json` / `dump_text` | NUL-terminated truncation at `buf_size - 1`; no chunking. Caller must size the buffer for the full document (the S3 exporter calls in a small loop with the prefix+offset to handle this). |
| `CONFIG_METRICS_ENABLED=n` | Macros compile to no-ops; non-macro callers link against stub impls that ignore inputs and return zeroed snapshots. |
| Counter overflow | `uint64_t` saturates per IEEE-754 wrap rules; not currently checked. |
| Value min/max race | A late writer can briefly observe a non-monotonic min/max while the CAS retries; convergence is reached on the next write. |

There are no allocations to fail, no locks to time out on, and no
recoverable errors at the API surface.

## Integration example

```cpp
#include "metrics/metrics_macros.h"
#include "metrics/metrics.h"

void chip_init() {
    metrics::init();                 // once at boot
}

void hap_rx_handler(const Frame& f) {
    _METRIC_TIMER_SCOPE(METRIC_HAP_RX_HANDLE);
    _METRIC_COUNTER_INC(METRIC_HAP_RX_FRAMES_TOTAL, 1);
    // ... decode + dispatch ...
}

void on_periodic_metrics_publish() {
    metrics::update_memory_snapshot();
    char buf[2048];
    const size_t n = metrics::mqtt_format_snapshot_json(buf, sizeof(buf));
    if (n) mqtt_publish("zhac/metrics", buf, n);
}

esp_err_t prometheus_handler(httpd_req_t* req) {
    metrics::update_memory_snapshot();
    char buf[4096];
    size_t pos = 0;
    pos += metrics::prometheus_format(buf + pos, sizeof(buf) - pos);
    httpd_resp_set_type(req, "text/plain; version=0.0.4");
    return httpd_resp_send(req, buf, pos);
}
```

Adding a new metric:

```c
// include/metrics/metric_registry.def
METRIC_COUNTER(METRIC_RULES_FIRED_TOTAL, "rules_fired_total", "Simple rules fired")
```

Then call `_METRIC_COUNTER_INC(METRIC_RULES_FIRED_TOTAL, 1)` from the
firing site. The descriptor table grows automatically; the
`static_assert` in `metrics.cpp` catches drift.

## Constraints (do not relax without discussion)

- No heap allocation anywhere in the core or the exporters.
- No `std::string`, `std::vector`, `std::unordered_map`,
  `std::shared_ptr`.
- All shard fields are `std::atomic` with `memory_order_relaxed`.
- Exporter formatters are bounded-buffer `snprintf` writers. No cJSON,
  no heap, no thread-local scratch.
- Readers walk shards with relaxed loads — small races are accepted.
- Disabled build (`CONFIG_METRICS_ENABLED=n`) provides stub impls so
  call sites that bypass the macros still link.

## Cross-references

- `docs/plans/2026-04-21-metrics-engine-plan.md` — original phased
  plan; useful historical context for the X-macro / sharding
  decisions.
- `zhac-net-core/main/rest_ops.cpp` — `/metrics` HTTP handler that
  drives `prometheus_format`.
- `zhac-net-core/main/metrics_mqtt.cpp` — periodic MQTT publisher
  driving `mqtt_format_snapshot_json`.
- `zhac-main-core/main/hap_dispatch.cpp` — METRICS_RSP / METRICS_REQ
  glue that ships P4's Prometheus blob to the S3 for cross-chip merge.
- `components/ws_server/README.md` — sibling shared-infrastructure
  component (S3 only).
