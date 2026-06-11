// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// components/event_bus/event_bus.cpp
#include "event_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char* TAG = "event_bus";

static constexpr uint8_t MAX_SUBS_PER_TYPE = 8;
static constexpr uint8_t EVENT_TYPE_COUNT  = static_cast<uint8_t>(EventType::_COUNT);
static constexpr uint8_t QUEUE_DEPTH       = 16;

// ── Subscription slots ────────────────────────────────────────────────────
//
// Slot lifecycle:   FREE ──subscribe──▶ ACTIVE ──unsubscribe──▶ FREE | DYING
//   FREE   queue == nullptr, !alive           (gen persists across reuse)
//   ACTIVE queue != nullptr,  alive
//   DYING  queue != nullptr, !alive — unsubscribed while publishes/drains
//          were in flight on the queue; the queue is deleted by reap_locked()
//          once `inflight` drops to 0. A DYING slot is not reusable until
//          reaped, so an in-flight drain can never confuse the old queue
//          with a new subscription's queue.
struct SubEntry {
    EventHandler  handler;
    QueueHandle_t queue;
    EventFilter   filter;    // optional — null means accept all events
    // Generation stamp, bumped on subscribe AND unsubscribe and embedded in
    // the returned handle. A stale handle (slot since unsubscribed and/or
    // reused) fails the gen check in unsubscribe/drain instead of acting on
    // the new occupant (pre-fix: stale double-unsubscribe deleted the new
    // subscriber's queue). ABA residual: uint8 wraps after 256 sub/unsub
    // cycles on the SAME slot, so a handle retained across exactly k*256
    // cycles could false-validate — accepted: in-tree code never even calls
    // unsubscribe today, every access is still memory-safe (gen+alive gate
    // queue use; queues are reaped via inflight), and widening gen would
    // break the 16-bit EventSubHandle ABI.
    uint8_t       gen;
    // Number of tasks currently using `queue` OUTSIDE the bus lock (a
    // publisher between snapshot and send, or a drainer inside
    // xQueueReceive). Modified only under the lock. reap_locked() deletes a
    // DYING slot's queue only when this is 0 — the drain-vs-unsubscribe
    // use-after-free fix (pre-fix: unsubscribe vQueueDelete'd the queue a
    // drainer was blocked on).
    uint8_t       inflight;
    bool          alive;
};

static SubEntry s_subs[EVENT_TYPE_COUNT][MAX_SUBS_PER_TYPE];
// s_sub_hwm[t] = highest used slot index + 1 for type t (DYING counts as used)
static uint8_t  s_sub_hwm[EVENT_TYPE_COUNT];
// Count of DYING slots across all types — gates the reap scan so the publish
// hot path pays one integer compare when nothing is pending deletion.
static uint8_t  s_dying = 0;
static bool     s_inited = false;

// ── Handle packing ────────────────────────────────────────────────────────
// handle = [15..11 type][10..3 gen][2..0 pos]. With type ≤ 15 (asserted) a
// valid handle never sets bit 15, so it can never collide with
// EVENT_SUB_INVALID (0xFFFF). Encoding is opaque to callers.
static_assert(EVENT_TYPE_COUNT <= 16,
              "EventType no longer fits the 4-bit handle field — repack EventSubHandle");
static_assert(MAX_SUBS_PER_TYPE <= 8,
              "MAX_SUBS_PER_TYPE no longer fits the 3-bit handle field");

static inline EventSubHandle pack_handle(uint8_t type, uint8_t gen, uint8_t pos) {
    return (EventSubHandle)(((uint16_t)type << 11) | ((uint16_t)gen << 3) | pos);
}
static inline uint8_t handle_type(EventSubHandle h) { return (uint8_t)((h >> 11) & 0x1F); }
static inline uint8_t handle_gen(EventSubHandle h)  { return (uint8_t)((h >> 3) & 0xFF); }
static inline uint8_t handle_pos(EventSubHandle h)  { return (uint8_t)(h & 0x07); }

// F28/F36 (FINDINGS.md): guard the subscriber table. subscribe/unsubscribe
// mutate it while publish/drain (called from many tasks) read it. The lock
// now covers only table access — never user callbacks or queue blocking:
// publish snapshots its targets and sends unlocked; drain blocks in
// xQueueReceive unlocked and re-validates before dispatch. Recursive so a
// handler/filter that re-enters the bus can't self-deadlock.
// Null-guarded for pre-init single-threaded use.
static SemaphoreHandle_t s_bus_mtx = nullptr;
static inline void bus_lock()   { if (s_bus_mtx) xSemaphoreTakeRecursive(s_bus_mtx, portMAX_DELAY); }
static inline void bus_unlock() { if (s_bus_mtx) xSemaphoreGiveRecursive(s_bus_mtx); }

// Lock held. Shrink the high-water mark past trailing FREE slots.
static void recalc_hwm_locked(uint8_t t) {
    while (s_sub_hwm[t] > 0) {
        const SubEntry& s = s_subs[t][s_sub_hwm[t] - 1];
        if (s.queue == nullptr && !s.alive) s_sub_hwm[t]--;
        else break;
    }
}

// Lock held. Delete the queues of DYING slots no task is using any more.
// Deletion is deferred here (instead of inside unsubscribe) because a
// drainer may be blocked in xQueueReceive on the queue: `inflight` is only
// raised under the lock, so inflight == 0 here proves no task is inside —
// or can newly enter (gen already bumped) — a queue op on the handle.
static void reap_locked() {
    if (s_dying == 0) return;
    for (uint8_t t = 1; t < EVENT_TYPE_COUNT && s_dying > 0; t++) {
        for (uint8_t i = 0; i < MAX_SUBS_PER_TYPE; i++) {
            SubEntry& s = s_subs[t][i];
            if (!s.alive && s.queue && s.inflight == 0) {
                vQueueDelete(s.queue);
                s.queue = nullptr;
                s_dying--;
                recalc_hwm_locked(t);
            }
        }
    }
}

void event_bus_init() {
    // Re-init guard (house style, cf. hap_session Q19): a second call must
    // not wipe the table — that leaked every live queue and silently
    // orphaned all subscriptions. Warn and keep state.
    if (s_inited) {
        ESP_LOGW(TAG, "re-init ignored — bus already initialised");
        return;
    }
    if (!s_bus_mtx) s_bus_mtx = xSemaphoreCreateRecursiveMutex();
    for (uint8_t t = 0; t < EVENT_TYPE_COUNT; t++) {
        s_sub_hwm[t] = 0;
        for (uint8_t i = 0; i < MAX_SUBS_PER_TYPE; i++) {
            s_subs[t][i].handler  = nullptr;
            s_subs[t][i].queue    = nullptr;
            s_subs[t][i].filter   = nullptr;
            s_subs[t][i].gen      = 0;
            s_subs[t][i].inflight = 0;
            s_subs[t][i].alive    = false;
        }
    }
    s_dying  = 0;
    s_inited = true;
    ESP_LOGI(TAG, "event_bus init OK");
}

EventSubHandle event_bus_subscribe(EventType type, EventHandler handler,
                                   EventFilter filter) {
    uint8_t idx = static_cast<uint8_t>(type);
    if (idx == 0 || idx >= EVENT_TYPE_COUNT) {
        ESP_LOGE(TAG, "invalid event type %d", idx);
        return EVENT_SUB_INVALID;
    }

    bus_lock();
    reap_locked();   // a DYING slot whose last user just left frees up here

    // Find first FREE slot (supports re-use after unsubscribe). DYING slots
    // still hold their queue, so the queue==nullptr test skips them.
    int8_t pos = -1;
    for (uint8_t i = 0; i < MAX_SUBS_PER_TYPE; i++) {
        if (s_subs[idx][i].queue == nullptr && !s_subs[idx][i].alive) {
            pos = (int8_t)i;
            break;
        }
    }
    if (pos < 0) {
        bus_unlock();
        ESP_LOGE(TAG, "subscriber table full for type %d", idx);
        return EVENT_SUB_INVALID;
    }

    QueueHandle_t q = xQueueCreate(QUEUE_DEPTH, sizeof(Event));
    if (!q) {
        // Pre-fix this was configASSERT-only: with asserts compiled out the
        // null queue was stored and the handler later ran synchronously
        // under the global lock. Fail the subscribe instead.
        bus_unlock();
        ESP_LOGE(TAG, "queue alloc failed for type %d", idx);
        return EVENT_SUB_INVALID;
    }

    SubEntry& s = s_subs[idx][pos];
    s.handler = std::move(handler);
    s.queue   = q;
    s.filter  = std::move(filter);
    s.gen++;            // new generation — stale handles to this slot die here
    s.alive   = true;   // inflight is 0 by the FREE-slot invariant
    const uint8_t gen = s.gen;
    if ((uint8_t)(pos + 1) > s_sub_hwm[idx])
        s_sub_hwm[idx] = (uint8_t)(pos + 1);
    bus_unlock();

    ESP_LOGI(TAG, "subscribed type=%d pos=%d gen=%u", idx, pos, gen);
    return pack_handle(idx, gen, (uint8_t)pos);
}

void event_bus_unsubscribe(EventSubHandle handle) {
    if (handle == EVENT_SUB_INVALID) return;
    const uint8_t idx = handle_type(handle);
    const uint8_t gen = handle_gen(handle);
    const uint8_t pos = handle_pos(handle);
    if (idx == 0 || idx >= EVENT_TYPE_COUNT || pos >= MAX_SUBS_PER_TYPE) return;

    bus_lock();
    SubEntry& s = s_subs[idx][pos];
    if (!s.alive || s.gen != gen) {
        // Stale or double unsubscribe — the slot was already freed and
        // possibly reused. Pre-fix this deleted the NEW occupant's queue.
        bus_unlock();
        ESP_LOGW(TAG, "stale unsubscribe ignored type=%d pos=%d", idx, pos);
        return;
    }
    s.alive   = false;
    s.gen++;             // outstanding handles (incl. this one) are now stale
    s.handler = nullptr; // drop user captures now —
    s.filter  = nullptr; // pre-fix the filter's captures leaked in the slot
    if (s.inflight == 0) {
        // Fast path: no publish/drain holds the queue, and none can newly
        // validate (gen bumped, alive false) while we hold the lock.
        vQueueDelete(s.queue);
        s.queue = nullptr;
        recalc_hwm_locked(idx);
    } else {
        // A drain may be blocked inside xQueueReceive on this very queue —
        // deletion is deferred to reap_locked() once inflight hits 0. Wake
        // any blocked drainer with a poison event so a portMAX_DELAY drain
        // on a now-dead subscription doesn't sleep until the next real
        // publish; it re-validates gen on wake and exits without
        // dispatching. Send may fail when the queue is full — fine: a full
        // queue wakes the drainer by itself.
        Event poison{};
        (void)xQueueSend(s.queue, &poison, 0);
        s_dying++;
    }
    bus_unlock();

    ESP_LOGI(TAG, "unsubscribed type=%d pos=%d", idx, pos);
}

void event_bus_publish(const Event& e) {
    uint8_t idx = static_cast<uint8_t>(e.type);
    if (idx == 0 || idx >= EVENT_TYPE_COUNT) return;

    // F28/F36 + P1-T11: snapshot the matching subscribers under the lock,
    // then evaluate filters and send OUTSIDE it. Pre-fix the lock was held
    // across the whole fan-out including user filter callbacks — one slow
    // filter stalled every publisher, and a filter that published re-entered
    // with the lock held (cross-task ordering deadlock risk; shadow's emit
    // publishes while holding its own mutex, so no new lock may nest here).
    // Queue handles are plain values; the filter std::function is COPIED,
    // but only when non-null — no in-tree subscriber sets one today, and
    // SSO-sized captures don't heap-allocate, so the per-publish cost on the
    // hot path (every device attr event) is nil. `inflight` pins each
    // snapshotted queue against deletion by a concurrent unsubscribe+reap.
    struct Target {
        QueueHandle_t q;
        EventFilter   filter;
        uint8_t       pos;
    };
    Target  targets[MAX_SUBS_PER_TYPE];
    uint8_t n = 0;

    bus_lock();
    for (uint8_t i = 0; i < s_sub_hwm[idx]; i++) {
        SubEntry& s = s_subs[idx][i];
        if (!s.alive || !s.queue) continue;   // FREE or DYING slot
        targets[n].q   = s.queue;
        targets[n].pos = i;
        if (s.filter) targets[n].filter = s.filter;   // copy only when set
        s.inflight++;
        n++;
    }
    bus_unlock();
    if (n == 0) return;

    for (uint8_t k = 0; k < n; k++) {
        if (targets[k].filter && !targets[k].filter(e)) continue;
        if (xQueueSend(targets[k].q, &e, 0) != pdTRUE) {
            // Eviction policy: overwrite-oldest (E3).
            // On queue full, the oldest event is silently discarded to make room
            // for the new one. This is intentional: for high-rate sensor bursts
            // (ZCL attribute updates, MQTT messages) the newest value is always
            // more relevant than a stale reading. Subscribers that cannot keep up
            // will see the latest state rather than a backlog of outdated events.
            // (Two publishers racing the evict+resend pair is benign: queue ops
            // are atomic; the net effect is each drops one oldest entry.)
            Event discard{};
            xQueueReceive(targets[k].q, &discard, 0);
            xQueueSend(targets[k].q, &e, 0);
            ESP_LOGW(TAG, "queue full type=%d sub=%d — oldest overwritten", idx, targets[k].pos);
        }
    }

    bus_lock();
    for (uint8_t k = 0; k < n; k++)
        s_subs[idx][targets[k].pos].inflight--;
    reap_locked();   // we may have been the last user of a dying queue
    bus_unlock();
}

// Drain core for one subscription slot. `gen` comes from the caller's
// handle and is re-validated after EVERY queue wake: the subscription may
// have been unsubscribed (and the slot even reused) while we were blocked
// in xQueueReceive, and a dead generation's events — poison or real — must
// never be dispatched. The handler is invoked on a private copy taken at
// entry, so its captures stay valid regardless; the copy is once per drain
// CALL (not per event), and for the in-tree plain-function handlers it
// doesn't touch the heap.
static uint8_t drain_slot(uint8_t idx, uint8_t pos, uint8_t gen, uint32_t timeout_ms) {
    SubEntry& s = s_subs[idx][pos];

    bus_lock();
    if (!s.alive || s.gen != gen || !s.queue) {
        bus_unlock();
        return 0;
    }
    QueueHandle_t q       = s.queue;
    EventHandler  handler = s.handler;
    s.inflight++;   // pins q against vQueueDelete until we decrement
    bus_unlock();

    uint8_t    count = 0;
    TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
    Event      ev{};
    // Pre-fix this whole loop read s_subs and invoked the handler with no
    // lock at all — unsubscribe could vQueueDelete the queue we were blocked
    // on and null the std::function we were about to call.
    while (xQueueReceive(q, &ev, ticks) == pdTRUE) {
        ticks = 0;
        bus_lock();
        const bool live = s.alive && s.gen == gen;
        bus_unlock();
        if (!live) break;   // died while we were blocked — drop, don't dispatch
        // Invoke OUTSIDE the lock: a slow or re-publishing handler must not
        // stall publishers (simple_rules' dispatch_event republishes
        // RULE_EVENT into this very queue — it takes the lock fresh).
        if (handler) handler(ev);
        count++;
    }

    bus_lock();
    s.inflight--;
    reap_locked();   // we may have been the last user of a dying queue
    bus_unlock();
    return count;
}

uint8_t event_bus_drain_handle(EventSubHandle handle, uint32_t timeout_ms) {
    if (handle == EVENT_SUB_INVALID) return 0;
    const uint8_t idx = handle_type(handle);
    const uint8_t gen = handle_gen(handle);
    const uint8_t pos = handle_pos(handle);
    if (idx == 0 || idx >= EVENT_TYPE_COUNT || pos >= MAX_SUBS_PER_TYPE) return 0;
    return drain_slot(idx, pos, gen, timeout_ms);
}

// DEPRECATED wrapper — see header. Old semantics preserved: drains every
// subscription of the type, blocking timeout on the first queue only. Now
// built on the same gen+inflight machinery as event_bus_drain_handle, so a
// concurrent unsubscribe is safe (the stale slot just drains 0).
uint8_t event_bus_drain(EventType type, uint32_t timeout_ms) {
    uint8_t idx = static_cast<uint8_t>(type);
    if (idx == 0 || idx >= EVENT_TYPE_COUNT) return 0;

    uint8_t poss[MAX_SUBS_PER_TYPE];
    uint8_t gens[MAX_SUBS_PER_TYPE];
    uint8_t n = 0;

    bus_lock();
    for (uint8_t i = 0; i < s_sub_hwm[idx]; i++) {
        const SubEntry& s = s_subs[idx][i];
        if (s.alive && s.queue) {
            poss[n] = i;
            gens[n] = s.gen;
            n++;
        }
    }
    bus_unlock();

    uint8_t count = 0;
    for (uint8_t k = 0; k < n; k++)
        count += drain_slot(idx, poss[k], gens[k], (k == 0) ? timeout_ms : 0);
    return count;
}
