// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstdint>
#include <functional>
#include "zcl_attribute.h"

enum class EventType : uint8_t {
    DEVICE_JOIN      = 1,
    DEVICE_LEAVE     = 2,
    ATTR_CHANGE      = 3,
    ZCL_ATTR         = 3,   // alias for ATTR_CHANGE
    ZCL_CMD          = 4,
    RULE_TRIGGER     = 5,
    CTRL_BOOT        = 6,   // coordinator boot completed
    ZCL_RAW          = 7,   // unmatched ZCL frame (no converter def)
    MQTT_MSG         = 8,   // MQTT message received
    RULE_EVENT       = 9,   // custom named event (rule chaining)
    RULE_TIMER_FIRE  = 10,  // rule timer N fired
    // Optimistic (command-driven, unconfirmed) shadow change. Carries the
    // ZclAttrEvent payload, same as ZCL_ATTR, but on a distinct type so the
    // HAP forwarder relays it to the S3 gateway (→ local webui + cloud) while
    // the rule engine — which subscribes to ZCL_ATTR only — never fires on an
    // unconfirmed value. Emitted by device_shadow_update_optimistic() for
    // no-report devices (Tuya LED drivers) that otherwise never reflect a
    // command past the P4 cache.
    SHADOW_OPTIMISTIC = 11,

    // Sentinel — one past the highest valid type. Sizes the subscriber
    // table (event_bus.cpp); NOT a publishable type. Keep last. Source-only
    // addition: no payload struct references it, so the 96-byte event ABI
    // is untouched.
    _COUNT,
};

// ── ZCL_ATTR payload (one attribute per event) — string-keyed, 96 B ──────
// v2 layout: legacy attr_keys integer IDs replaced by inline string keys +
// inline string values.
struct __attribute__((packed)) ZclAttrEvent {
    uint64_t ieee;               // 0-7
    uint16_t nwk;                // 8-9
    uint8_t  ep;                 // 10
    uint8_t  val_type;           // 11   ValType
    uint16_t cluster;            // 12-13
    uint16_t attr_id;            // 14-15
    char     key[ATTR_KEY_MAX];  // 16-43 (ATTR_KEY_MAX=28 at schema v6)
    union {                      // 44-91 (ATTR_STR_MAX=48 at schema v6)
        int32_t int_val;                 // INT/BOOL
        char    str_val[ATTR_STR_MAX];   // STR
    };
    uint8_t  _pad[4];            // 92-95 (keeps the 96-byte event-bus contract)
};
static_assert(sizeof(ZclAttrEvent) == 96);

// ── ZCL_RAW payload — unmatched ZCL frame — fits in data[96] ─────────────
struct __attribute__((packed)) ZclRawEvent {
    uint64_t ieee;         // source device IEEE address
    uint16_t nwk;          // source device NWK address
    uint8_t  ep;           // source endpoint
    uint8_t  command;      // ZCL command ID
    uint16_t cluster;      // ZCL cluster ID
    uint8_t  payload_len;  // raw ZCL payload length (capped at 80)
    uint8_t  _pad;
    char     payload_hex[80]; // raw ZCL payload as hex string, null-terminated
};
static_assert(sizeof(ZclRawEvent) == 96);

// ── MQTT_MSG payload ─────────────────────────────────────────────────────
struct __attribute__((packed)) MqttMsgEvent {
    char topic[64];    // MQTT topic, null-terminated (truncated to fit)
    char payload[32];  // MQTT payload, null-terminated (truncated to fit)
};
static_assert(sizeof(MqttMsgEvent) == 96);

// ── RULE_EVENT payload ────────────────────────────────────────────────────
struct RuleEventPayload {
    char    name[95]; // custom event name
    // TTL hop counter: 0 = external origin; each rule republish copies
    // hop+1 into the new payload; simple_rules cuts the chain at
    // MAX_EVENT_HOPS (full rationale at simple_rules.cpp:25).
    uint8_t hop;
};
static_assert(sizeof(RuleEventPayload) == 96);

// ── RULE_TIMER_FIRE payload ───────────────────────────────────────────────
struct RuleTimerPayload {
    uint8_t  timer_index; // 1–8
    uint8_t  _pad[95];
};
static_assert(sizeof(RuleTimerPayload) == 96);

struct Event {
    EventType type;
    alignas(8) uint8_t   data[96];
};

using EventHandler = std::function<void(const Event&)>;

// Optional predicate for event_bus_subscribe. When non-null, only events for
// which the filter returns true are enqueued / dispatched to the subscriber.
using EventFilter = std::function<bool(const Event&)>;

// Opaque subscription handle returned by event_bus_subscribe.
// Pass to event_bus_unsubscribe to remove the subscription.
// EVENT_SUB_INVALID indicates a failed subscribe.
using EventSubHandle = uint16_t;
static constexpr EventSubHandle EVENT_SUB_INVALID = 0xFFFF;

void           event_bus_init();

// Publish an event to every live subscriber of e.type. Per-subscriber FIFO
// ordering; on a full queue the OLDEST event is overwritten (newest wins).
// Never blocks. Subscriber filters run here, in the publisher's task — keep
// them fast. An invalid/sentinel type is a silent no-op.
void           event_bus_publish(const Event& e);

// Subscribe to events of the given type. Optionally supply a filter predicate
// so the subscriber only receives matching events (e.g. a specific IEEE address).
// The filter is COPIED into the publish snapshot on every matching publish and
// runs outside the bus lock — keep captures small (SSO-sized) and non-blocking.
// Returns EVENT_SUB_INVALID on bad type, full table, or queue allocation
// failure. Handles are generation-stamped: a handle outlives its subscription
// only as a safely-rejected stale value.
EventSubHandle event_bus_subscribe(EventType type, EventHandler handler,
                                   EventFilter filter = nullptr);

// Remove a subscription. Generation-checked: a stale or double unsubscribe is
// ignored (logged). Contract:
//   - After return, no NEW handler/filter invocation begins for this handle
//     (drains re-validate the generation before dispatching). One delivery
//     that already passed validation concurrently with this call may still
//     complete — it runs on a private std::function copy, so captures stay
//     valid for that delivery even though the slot's handler/filter are
//     cleared here.
//   - The backing queue may outlive this call while drains/publishes are
//     in flight on it; it is reclaimed (vQueueDelete) by a later
//     publish/subscribe/drain once no task can be inside a queue op.
//   - A drain blocked on this subscription's queue is woken with a poison
//     event and returns without dispatching it.
void           event_bus_unsubscribe(EventSubHandle handle);

// Drain queued events for ONE subscription — call from the subscriber's own
// task. Returns the number of events dispatched; 0 for a stale/invalid handle.
// Blocks up to timeout_ms for the first event, then drains greedily.
// Safe against concurrent unsubscribe (generation re-validated after every
// queue wake; the queue cannot be deleted while a drain is inside it).
// Intended one drainer per subscription (the owning task); concurrent drains
// on the same handle compete for events.
uint8_t event_bus_drain_handle(EventSubHandle handle, uint32_t timeout_ms);

// DEPRECATED — drain ALL subscriptions of a type in the calling task; with
// 2+ subscribers their handlers all execute here, in whatever task drains
// first, and the blocking timeout applies only to the first queue. The full
// timeout may be consumed blocking on an empty first queue while later
// queues already hold events. Kept for
// the single-dispatcher main loop (P4 task_event_bus); new code should keep
// its EventSubHandle and use event_bus_drain_handle(). Comment-only
// deprecation: [[deprecated]] would -Werror existing in-tree callers.
// Returns number of events processed across all subscriptions.
uint8_t event_bus_drain(EventType type, uint32_t timeout_ms);
