# simple_rules — DSL Rule Engine (P4)

Embedded `ON … DO … ENDON` automation engine: parses a small DSL,
holds the parsed rules in PSRAM, and dispatches actions when matching
EventBus events fire. Persistence is handled by `rule_store`; this
component owns parsing, matching, dispatch, and the once-per-minute
cron task.

## Where it sits

```
REST / WS  ──► simple_rules_add/update/delete ──► rule_store (NVS)
                            │
                            ▼
EventBus(ZCL_ATTR/MQTT/CTRL_BOOT/RULE_EVENT/RULE_TIMER_FIRE)
                            │
                            ▼
                    dispatch_event()  ── (snapshot) ──► execute_rule()
                                                        │
                                ┌───────────────────────┼───────────────────────┐
                                ▼                       ▼                       ▼
                       zigbee_mgr_zcl_set        mqtt_publish          event_bus_publish
                                                                       (RULE_EVENT/TIMER)
                                                                       │
                                                                       └─► simple_rules_set_script_hook()
                                                                            (lua_engine optional)
```

P4 only. The S3 forwards REST/WS rule CRUD to P4 over HAP.

### Dependencies (`CMakeLists.txt` REQUIRES)

`zap_common` `event_bus` `rule_store` `cron_parser` `zigbee_mgr`
`mqtt_client` `freertos` `nvs_flash` `metrics`. The Lua hook is
**optional and decoupled**: `simple_rules_set_script_hook` lets
`lua_engine` register a callback so this component never depends on
the scripting engine.

## DSL at a glance

```
ON System#Boot DO log controller up ENDON
ON Time#Cron=0 */5 * * * * DO event heartbeat ENDON
ON sensor#action="single" DO zigbee.set hallway_light state 1 ; timer 1 300000 ENDON
ON Rules#Timer=1 DO zigbee.set hallway_light state 0 ENDON
ON door#contact DO zigbee.set hallway_light state !%value% ENDON
ON room#temperature DO publish home/temp/c %value%/100 ENDON
```

Full grammar in `zhac-docs/RULES_DSL.md`. Triggers: `DEVICE_ATTR`,
`TIME_CRON`, `BOOT`, `EVENT`, `TIMER`, `MQTT_TOPIC`. Actions:
`ZIGBEE_SET`, `ZIGBEE_TOGGLE`, `PUBLISH`, `EVENT`, `TIMER`, `LOG`, `SCRIPT`.
Comparison ops: `ANY` *(no operator)*, `=`, `!=`, `>`, `<`, `>=`, `<=`.
Action device names are a single token — quotes are NOT stripped.

`zigbee.set` values and `publish` payloads accept `%value%` (the trigger
value) or an integer expression over it — `!%value%`, `%value%/100`,
`(%value%*10)/3+5` — compiled once at rule save by `expr_eval.cpp` (caps
48 chars / 12 ops / 6 parens reject at save; a runtime zero divisor or a
non-numeric trigger value skips the action). See `zhac-docs/RULES_DSL.md`
§"Value substitution & expressions".

## Public API (`include/simple_rules.h`)

### Lifecycle

| Symbol | Notes |
|---|---|
| `void simple_rules_init()` | Allocates `ParsedRule[MAX_CACHED_RULES=64]` in PSRAM, creates the recursive mutex, subscribes to the 5 driving event types, calls `reload_locked()`, spawns `task_cron`. |
| `void simple_rules_reload()` | Re-reads all slots from `rule_store` and re-resolves friendly names → IEEE. Call after device pool changes. |

### Rule CRUD

| Symbol | Notes |
|---|---|
| `bool simple_rules_add(const char* name, const char* dsl, uint16_t* out_rule_id)` | Parse, persist, in-memory append. `name` is an optional ≤23-char display label. Returns false on parse failure — see `dsl_last_error()`. |
| `bool simple_rules_update(uint16_t rule_id, const char* name, const char* dsl)` | Edit-in-place. |
| `bool simple_rules_delete(uint16_t rule_id)` | NVS deletion is queued via `rule_store_mark_delete`. |
| `bool simple_rules_enable(uint16_t rule_id, bool enabled)` | Persists the flag, no reparse. |
| `uint16_t simple_rules_list(RuleSlot* out, uint16_t max_count)` | Snapshot of all stored slots. |

### Parser surface (also used by tests)

| Symbol | Notes |
|---|---|
| `ParseResult dsl_parse(const char* dsl, uint16_t rule_id, ParsedRule* out)` | Pure parser. Does not resolve friendly names. |
| `const char* dsl_last_error()` | Human-readable error string for the most recent `dsl_parse` call. Used by HAP `RULE_*` handlers to propagate UI errors. |
| `void simple_rules_resolve_names(ParsedRule* rules, uint16_t count)` | Convert quoted device names → IEEE; sets `trigger.ieee` to 0 if unresolved. |
| `bool simple_rules_match(const ParsedRule&, const Event&, char* out_val, size_t cap)` | Match predicate; `out_val` receives the stringified event value for substitution. |

### Hooks

| Symbol | Notes |
|---|---|
| `void simple_rules_set_script_hook(simple_rules_script_hook_t)` | Registered by `lua_engine_init` so `DO script.run "<name>"` enqueues a TaskLua run without an upstream link. |
| `void simple_rules_set_error_cb(rules_error_cb_t)` | Called when a stored rule fails to reparse during reload (e.g. firmware upgrade changed grammar). |

## Important constants & sizes

| Symbol | Value | Source |
|---|---|---|
| `MAX_CACHED_RULES` | 64 | **active-rule cap** — the in-memory `ParsedRule` array. See "Active-rule limit" below. |
| `ZAP_MAX_RULES` | 256 | `zap_common.h` — `rule_store` NVS slot capacity (persistence, not evaluation) |
| `MAX_EVENT_HOPS` | 8 | TTL for rule→rule `RULE_EVENT` chains, carried per-payload (`RuleEventPayload.hop`) — cuts self-feeding loops |
| `MAX_CRON_FIRES` | 16 | cap on rules that can fire in one minute |
| `RuleAction.arg{0,1,2}` | 32/20/20 | scratch space for device ref / payload / value |
| `RuleAction.expr` | `ExprProg`, ≤12 RPN ops | compiled `%value%` expression (`expr_eval.h`) — set when the value arg is an expression; transient (PSRAM), never persisted |
| `ParsedRule` | 4 actions per rule | `actions[4]` array |
| Mutex acquire timeout | 2 s (cron snapshot) / 500 ms (per-fire), `portMAX_DELAY` only for init/reload |

### Active-rule limit (cache 64 vs store 256)

The engine keeps at most **`MAX_CACHED_RULES` (64)** rules *active* (parsed,
in PSRAM, evaluated on every event), while `rule_store` persists up to
**`ZAP_MAX_RULES` (256)** rules in NVS. The cap is deliberate: a 64-entry
`ParsedRule` array is the budget the P4's internal DRAM allows; raising it to
256 would cost roughly +130 KB and is not done here.

Consequences callers must know:

- **`simple_rules_add` rejects** (returns `false`, sets `dsl_last_error()` to
  `"rule cache full (max 64 active rules)"`) once 64 rules are active —
  it no longer persists-but-ignores a 65th rule. (P2-T18 def 2)
- **On `reload`**, if NVS holds more rules than the cache can hold, the
  excess are loaded in NVS-iteration order until the cache fills; the
  remainder are persisted but **not evaluated**. `reload_locked` logs an
  `ESP_LOGW` with the skipped count so this is never silent. (P2-T18 def 3)
- **New rule ids** are derived from `rule_store_max_id()` (the highest id
  across *all* 256 persisted slots + the writeback overlay), never from the
  64-entry cache — a persisted-but-uncached rule's id can't be reissued and
  silently overwritten. (P2-T18 def 1)

### Input bounds

- **DSL length:** `simple_rules_add` / `simple_rules_update` reject any DSL
  `>= sizeof(RuleSlot::src)` (500 B; effective max 499). No truncate-persist —
  the live and stored rule can never diverge. (P2-T18 def 4)
- **Action section:** if the text between `DO` and `ENDON` exceeds the
  500-byte parse buffer, `dsl_parse` returns `ERR_ACTION_TOO_LONG` instead of
  clamping to a differently-parsed action set. (P2-T18 def 4)
- **Numeric literals:** out-of-range / non-finite literals (`1e20`, garbage)
  are rejected (`ERR_BAD_TRIGGER`) before the `int32_t` cast — no UB. (def 5)

## Wire format / on-disk layout

Persistence delegated to `rule_store`. One `RuleSlot` (536 B) per rule:
`rule_id(2) + enabled(1) + _reserved(1) + src_len(2) + trigger_type(1) +
rule_type(1) + name(24) + src[500] + crc32(4)`. See
`components/rule_store/README.md`.

`ParsedRule` itself is RAM-only (PSRAM); the parser builds it from
`RuleSlot.src` on every reload.

## Threading & concurrency

- **One internal task:** `task_cron`, priority 2, 2 KB stack. Wakes
  every 60 s, scans active TIME_CRON rules, dispatches matches.
- **One recursive mutex** (`s_mutex`) protects the parsed-rule
  array. Recursive so an action that publishes `RULE_EVENT` can
  re-enter `dispatch_event` cleanly.
- **Snapshot-then-exec pattern (LUA-F8 / SR-F8 / CC-F5).**
  Both `dispatch_event` and `task_cron` collect the firing rule
  *indices* under a timeout-bounded `xSemaphoreTakeRecursive`, drop
  the lock, then take a per-rule short snapshot under the lock and
  release it before calling `execute_rule`. Action dispatch
  (zigbee_mgr_zcl_set, mqtt_publish, event_bus_publish, script hook)
  runs **without holding the mutex** so a stalled SPI queue or slow
  MQTT broker no longer freezes the whole engine. The 2 s cron
  acquire timeout logs `"task_cron: s_mutex contended >2s — minute
  skipped"` rather than hanging the cron task.
- **Rule-event loop TTL.** `RULE_EVENT` delivery is queue-based, so a
  self-feeding rule (`ON Event#x DO event x ENDON`) re-enqueues into
  the queue `event_bus_drain` is draining — a dispatch-depth counter
  never trips because each hop is a fresh dispatch at depth 0. Instead
  every `RuleEventPayload` carries a `hop` counter (0 = external
  origin); the `event` action refuses to republish once the chain
  reaches `MAX_EVENT_HOPS` and logs a warning.

## Failure modes

| Condition | Behaviour |
|---|---|
| DSL parse error in `add` / `update` | Returns false; error string available via `dsl_last_error()` |
| Stored rule fails to reparse on reload | `rules_error_cb_t` fired (UI logs / clears the rule); slot stays in NVS |
| `MAX_CACHED_RULES` (64 active) reached | `add` returns false; `dsl_last_error()` = "rule cache full (max 64 active rules)" |
| Persisted rule count > 64 on reload | Excess persisted-but-not-evaluated; `reload_locked` logs `ESP_LOGW` with skipped count |
| DSL ≥ 500 B in `add`/`update` | Rejected; `dsl_last_error()` = "rule DSL too long (max 499 bytes)" (no truncate-persist) |
| Action target device not found | `zigbee_mgr_zcl_set` returns error; logged; other actions continue |
| Cron rule with broken expression | `cron_parse` failure during `task_cron` is a per-iteration skip — never aborts the task |
| Mutex contended >2 s in cron | Minute skipped; logs warning |
| `RULE_EVENT` chain reaches `MAX_EVENT_HOPS` | Republish dropped (loop cut); logs warning with rule id |

## Integration example

```c
// Boot order:
rule_store_init();
rule_store_flush_init();
event_bus_init();
zigbee_mgr_init();
simple_rules_init();
lua_engine_init();    // optional — installs the script hook

// Add a rule from REST / WS:
uint16_t id;
if (!simple_rules_add("Hallway motion lights",
                      "ON HallSensor.occupancy == 1 "
                      "DO zigbee.set \"hall light\" state 1 "
                      "DO timer 0 300000 ENDON",
                      &id)) {
    rest_reply_error(dsl_last_error());
}
```

## Testing

Host harness in `test/host/` (plain cmake + ctest, FreeRTOS/ESP shims) covers the rule-event TTL loop cut.

## Recent changes

- **2026-04-25 snapshot-then-exec pattern.** Both `dispatch_event`
  and `task_cron` now drop the rule mutex before action dispatch
  (LUA-F8 + SR-F8 + CC-F5 in `docs/FINDINGS.md`). Mutex acquire is
  timeout-bounded; cron skips a minute rather than blocking forever.
- **`SCRIPT` action + `simple_rules_set_script_hook`.** New `DO
  script.run "<name>"` lets rules invoke Lua coroutines. The hook
  pattern keeps `simple_rules` free of a `lua_engine` dependency.
- **`name` field added to `RuleSlot`** (2026-04-22): friendly UI
  label persisted alongside the DSL source. `RuleSlot` grew 512 → 536 B.

## Cross-references

- `docs/RULES_DSL.md` — full grammar, examples, error catalogue
- `docs/FINDINGS.md` — SR-F1…SR-F8 (notably SR-F8 — the snapshot fix)
- `components/rule_store/README.md` — persistence layer
- `components/cron_parser/README.md` — TIME_CRON evaluation
- `components/event_bus/README.md` — event taxonomy
- `components/lua_engine/README.md` — script hook recipient
