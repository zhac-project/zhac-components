# event_bus — In-Process Pub/Sub (P4)

Lightweight FreeRTOS-friendly event bus that decouples producers
(zigbee_mgr, MQTT client, simple_rules, lua_engine bridge, HAP
session) from consumers. Zero-allocation: all 11 event types share a
fixed-size 96-byte payload and the bus itself is a static array of
subscriber slots — no malloc on the hot path, no dynamic dispatch.

## Where it sits

P4 core only. Producers call `event_bus_publish(EventType, &payload)`;
the bus iterates the subscriber slot table for that type and invokes
each registered `EventHandler` synchronously **on the publisher's
task**. Subscribers that need their own task pass a `QueueHandle_t`
into `event_bus_subscribe_queue` instead and drain at leisure.

### Dependencies (`CMakeLists.txt` REQUIRES)

`zap_common` `freertos` `esp_common` `log`. Header pulls
`zcl_attribute.h` for the `ATTR_STR_MAX` constant used inside
`ZclAttrEvent`.

## Event taxonomy (`EventType`)

| Value | Name | Producer | Payload |
|---|---|---|---|
| 1 | `DEVICE_JOIN` | zigbee_mgr | (free) |
| 2 | `DEVICE_LEAVE` | zigbee_mgr | (free) |
| 3 | `ATTR_CHANGE` / `ZCL_ATTR` | device_shadow | `ZclAttrEvent` |
| 4 | `ZCL_CMD` | zigbee_mgr | (free) |
| 5 | `RULE_TRIGGER` | simple_rules | (free) |
| 6 | `CTRL_BOOT` | controller startup | none — boot signal |
| 7 | `ZCL_RAW` | zigbee_mgr (no ZHC match) | `ZclRawEvent` |
| 8 | `MQTT_MSG` | mqtt_client | `MqttMsgEvent` |
| 9 | `RULE_EVENT` | simple_rules `event` action | `RuleEventPayload` |
| 10 | `RULE_TIMER_FIRE` | simple_rules timer expiry | `RuleTimerPayload` |

`ATTR_CHANGE == ZCL_ATTR == 3` (alias for callers that prefer the
older name).

## Public API (`include/event_bus.h`)

### Lifecycle

```c
void event_bus_init(void);   // zero subscriber table, log "init OK"
```

### Publish (sync, on caller's task)

```c
bool event_bus_publish(EventType type, const void* payload_96);
```

Synchronously walks the slot table for `type` and calls each
`EventHandler`. Returns `false` on invalid type. The 96-byte payload
is `memcpy`-ed into an `Event` on the caller's stack; subscribers may
hold pointers into that buffer only for the duration of the call.

### Subscribe

```c
EventSubHandle event_bus_subscribe(
    EventType    type,
    EventHandler handler,
    EventFilter  filter = nullptr);     // returns slot id, or EVENT_SUB_INVALID

EventSubHandle event_bus_subscribe_queue(
    EventType     type,
    QueueHandle_t target_queue);        // bus pushes Event into your queue
```

`EventFilter` is an optional `bool(*)(const Event*)` predicate called
*before* `handler`; returning `false` drops the event for that
subscriber only. Useful for cluster / IEEE filters.

### Unsubscribe / drain

```c
void event_bus_unsubscribe(EventSubHandle h);
size_t event_bus_drain_queue(QueueHandle_t q,
                             Event* out, size_t max);
```

`drain_queue` is the polite way for queue-based subscribers to pull
events in batches inside their own task loop.

## Important constants & sizes

| Symbol | Value | Source |
|---|---|---|
| `MAX_SUBS_PER_TYPE` | 8 | per-type slot count (file-scope, `event_bus.cpp`) |
| `EVENT_TYPE_COUNT` | 11 | matches `EventType` enum |
| `QUEUE_DEPTH` | 16 | depth of internal trampoline queue |
| `sizeof(Event)` | 104 | `EventType` + 8-aligned `data[96]` |

`s_subs[EVENT_TYPE_COUNT][MAX_SUBS_PER_TYPE]` is the only state — no
heap, no init order surprises beyond `event_bus_init`.

## Payload layouts (all 96 B, packed where it matters)

`ZclAttrEvent` (96 B, packed) — at v6 schema:

| Offset | Field |
|---|---|
| 0–7 | `uint64_t ieee` |
| 8–9 | `uint16_t nwk` |
| 10  | `uint8_t  ep` |
| 11  | `uint8_t  val_type` |
| 12–13 | `uint16_t cluster` |
| 14–15 | `uint16_t attr_id` |
| 16–43 | `char key[28]` |
| 44–47 / 44–91 | union `int32_t int_val` / `char str_val[48]` |
| 92–95 | `_pad[4]` |

`ZclRawEvent` (96 B): `ieee(8) + nwk(2) + ep(1) + command(1) +
cluster(2) + payload_len(1) + payload_hex[80]`.

`MqttMsgEvent` (96 B): `topic[64]` + `payload[32]`.

`RuleEventPayload` (96 B): `name[96]`.

`RuleTimerPayload` (96 B): `timer_index(1)` + `_pad[95]`.

The 96-byte ABI is enforced bus-wide so any subscriber can read any
type by `memcpy` without size negotiation.

## Threading & concurrency

- **No internal task.** `publish` runs handlers on the caller's task.
- Subscriber list is mutated only at init / explicit
  `(un)subscribe` — no locking on the publish path.
- `subscribe_queue` uses `xQueueSend` with 0 wait — full-queue
  events drop silently. Subscribers that care should size their
  queue appropriately.
- Filters and handlers must be reentrant — the same producer task
  may publish nested events (e.g. `simple_rules` → `RULE_EVENT` →
  another rule).

## Failure modes

| Condition | Behaviour |
|---|---|
| `subscribe` past slot 7 for a type | Returns `EVENT_SUB_INVALID`, logs the type id |
| Invalid `EventType` (0 or ≥ 11) | `subscribe` / `publish` return error, logs |
| Handler throws (would-be C++ exception) | Build is `-fno-exceptions`; bug → crash. Don't |
| Queue subscriber's queue full | `xQueueSend` returns `pdFALSE`; event for that subscriber dropped silently — caller's responsibility to size |

## Integration example

```c
// Init at app_main:
event_bus_init();

// Subscribe with filter:
static bool battery_only(const Event* e) {
    auto* a = reinterpret_cast<const ZclAttrEvent*>(e->data);
    return strncmp(a->key, "battery", ATTR_KEY_MAX) == 0;
}
event_bus_subscribe(EventType::ATTR_CHANGE, on_battery, battery_only);

// Publish:
ZclAttrEvent ev{};
ev.ieee     = dev->ieee_addr;
ev.cluster  = 0x0001;
ev.val_type = VAL_INT;
strncpy(ev.key, "battery", ATTR_KEY_MAX-1);
ev.int_val  = 87;
event_bus_publish(EventType::ATTR_CHANGE, &ev);
```

## Recent changes

- **Schema v6** widened `ZclAttrEvent.key` to 28 chars and inline
  string slot to 48 chars (mirrors `ShadowAttr`); event size kept at
  96 B by trimming reserved padding.
- New `RULE_TIMER_FIRE` (10) and `RULE_EVENT` (9) replaced legacy
  `RuleTimer` / `RuleEvent` IDs that overlapped with internal HAP
  signals.

## Cross-references

- `components/device_shadow/README.md` — primary `ZCL_ATTR` producer
- `components/simple_rules/README.md` — primary subscriber, also
  publishes `RULE_EVENT` / `RULE_TIMER_FIRE`
- `components/lua_engine/src/lua_engine_event_bridge.cpp` — bridges
  `ATTR_CHANGE` / `MQTT_MSG` / `CTRL_BOOT` into TaskLua
- `docs/FINDINGS.md` — see SR-F8, LUA-F8 for back-pressure caveats
