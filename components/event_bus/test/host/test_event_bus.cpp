// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host regression tests for event_bus concurrency hardening (P1-T11):
// generation-stamped handles, publish snapshot fan-out (filters copied,
// evaluated outside the bus lock), per-subscription drain, and the
// drain-vs-unsubscribe use-after-free fix (deferred queue reap via the
// per-slot inflight count).
//
// The shims are single-threaded, so true cross-task interleavings can't be
// scheduled; instead each unlocked window is driven from within itself:
//   - "unsubscribe between enqueue and drain"  → publish, unsubscribe, drain
//     (exercises the entry-time gen check on a fast-path-deleted slot);
//   - "unsubscribe while drain is mid-flight"  → the HANDLER unsubscribes
//     its own subscription. The drain holds inflight>0 across the handler
//     call, so this lands exactly in the unlocked window: deletion must
//     defer (DYING), the next received event must fail gen re-validation
//     and not dispatch, and drain exit must reap the queue.
// Not coverable single-threaded: a DYING slot surviving past drain exit
// (reap-on-next-publish then runs its s_dying_count==0 fast path only).
#include "event_bus.h"
#include "freertos/queue.h"   // host shim instrumentation

#include <cstdio>
#include <cstring>

static int s_failures = 0;
#define CHECK(cond, msg) do {                                         \
    if (cond) { printf("PASS: %s\n", msg); }                          \
    else      { printf("FAIL: %s\n", msg); s_failures++; }            \
} while (0)

// ── EVENT_TYPE_COUNT derives from the enum sentinel (fix 6) ──────────────
// 11 = types 1..10 plus the unused 0 slot. If a type is added without
// growing the table, this trips at compile time now instead of silently
// truncating the subscriber table at runtime.
static_assert(static_cast<uint8_t>(EventType::_COUNT) == 11,
              "EventType::_COUNT must track the enum (was hand-maintained 11)");

static Event make_event(EventType t, uint8_t tag) {
    Event ev{};
    ev.type    = t;
    ev.data[0] = tag;
    return ev;
}

int main() {
    event_bus_init();

    // ── 1. subscribe → publish → drain_handle roundtrip ───────────────────
    {
        int     calls = 0;
        uint8_t last  = 0;
        EventSubHandle h = event_bus_subscribe(
            EventType::DEVICE_JOIN,
            [&](const Event& e) { calls++; last = e.data[0]; });
        CHECK(h != EVENT_SUB_INVALID, "subscribe returns a valid handle");

        event_bus_publish(make_event(EventType::DEVICE_JOIN, 0x11));
        event_bus_publish(make_event(EventType::DEVICE_JOIN, 0x22));
        uint8_t n = event_bus_drain_handle(h, 0);
        CHECK(n == 2, "drain_handle dispatches both queued events");
        CHECK(calls == 2 && last == 0x22, "handler saw both events in order");

        // empty queue + nonzero timeout: shim receive never blocks → returns 0
        CHECK(event_bus_drain_handle(h, 50) == 0, "drain on empty queue returns 0");
        event_bus_unsubscribe(h);
    }

    // ── 2. publish snapshot honours the filter copy ───────────────────────
    {
        int calls = 0;
        uint8_t seen = 0;
        EventSubHandle h = event_bus_subscribe(
            EventType::DEVICE_LEAVE,
            [&](const Event& e) { calls++; seen = e.data[0]; },
            [](const Event& e) { return e.data[0] == 0x42; });
        CHECK(h != EVENT_SUB_INVALID, "filtered subscribe ok");

        event_bus_publish(make_event(EventType::DEVICE_LEAVE, 0x13)); // filtered out
        event_bus_publish(make_event(EventType::DEVICE_LEAVE, 0x42)); // passes
        uint8_t n = event_bus_drain_handle(h, 0);
        CHECK(n == 1 && calls == 1 && seen == 0x42,
              "filter admits matching event only (evaluated from the snapshot)");
        event_bus_unsubscribe(h);
    }

    // ── 3. stale handle after slot reuse is rejected (gen, fix 3) ─────────
    {
        int old_calls = 0, new_calls = 0;
        EventSubHandle h1 = event_bus_subscribe(
            EventType::ZCL_CMD, [&](const Event&) { old_calls++; });
        event_bus_unsubscribe(h1);

        // Slot freed on the fast path (no drain in flight) — reused here.
        EventSubHandle h2 = event_bus_subscribe(
            EventType::ZCL_CMD, [&](const Event&) { new_calls++; });
        CHECK(h2 != EVENT_SUB_INVALID && h2 != h1,
              "slot reuse yields a different (re-generationed) handle");

        // Pre-fix this deleted the NEW subscriber's queue.
        event_bus_unsubscribe(h1);
        event_bus_publish(make_event(EventType::ZCL_CMD, 0x01));
        CHECK(event_bus_drain_handle(h2, 0) == 1 && new_calls == 1,
              "stale double-unsubscribe did not kill the new subscription");
        CHECK(event_bus_drain_handle(h1, 0) == 0,
              "drain on a stale handle returns 0");
        CHECK(old_calls == 0, "old handler never ran");
        event_bus_unsubscribe(h2);
    }

    // ── 4. unsubscribe between enqueue and drain (gen check, fix 1) ───────
    {
        int calls = 0;
        EventSubHandle h = event_bus_subscribe(
            EventType::RULE_TRIGGER, [&](const Event&) { calls++; });
        event_bus_publish(make_event(EventType::RULE_TRIGGER, 0x05)); // queued
        event_bus_unsubscribe(h); // inflight==0 → queue deleted immediately
        CHECK(event_bus_drain_handle(h, 0) == 0 && calls == 0,
              "event enqueued before unsubscribe is dropped, handler never runs");
    }

    // ── 5. unsubscribe DURING drain: deferred reap + no post-unsub dispatch
    {
        int calls = 0;
        EventSubHandle h = EVENT_SUB_INVALID;
        h = event_bus_subscribe(
            EventType::CTRL_BOOT,
            [&](const Event&) {
                calls++;
                // Runs inside drain's unlocked window with inflight==1 —
                // exactly a cross-task unsubscribe racing a blocked drain.
                event_bus_unsubscribe(h);
            });
        event_bus_publish(make_event(EventType::CTRL_BOOT, 0x01));
        event_bus_publish(make_event(EventType::CTRL_BOOT, 0x02));

        uint8_t n = event_bus_drain_handle(h, 0);
        // Event 1 dispatches; its handler unsubscribes (deferred: DYING +
        // poison enqueued). Event 2 (and the poison) must fail the gen
        // re-validation and never reach the handler; drain exit reaps.
        CHECK(n == 1 && calls == 1,
              "after mid-drain unsubscribe, no further event is dispatched");

        // Queue was reaped at drain exit (inflight hit 0) → the slot is FREE
        // again: the type's full table must be subscribable.
        EventSubHandle hs[8];
        bool all_ok = true;
        for (int i = 0; i < 8; i++) {
            hs[i] = event_bus_subscribe(EventType::CTRL_BOOT, [](const Event&) {});
            if (hs[i] == EVENT_SUB_INVALID) all_ok = false;
        }
        CHECK(all_ok, "dying slot was reaped — all 8 slots subscribable again");
        for (int i = 0; i < 8; i++) event_bus_unsubscribe(hs[i]);
    }

    // ── 6. table full / slot recycling ─────────────────────────────────────
    {
        EventSubHandle hs[8];
        for (int i = 0; i < 8; i++)
            hs[i] = event_bus_subscribe(EventType::ZCL_RAW, [](const Event&) {});
        EventSubHandle h9 = event_bus_subscribe(EventType::ZCL_RAW, [](const Event&) {});
        CHECK(h9 == EVENT_SUB_INVALID, "9th subscribe on a full type fails");
        event_bus_unsubscribe(hs[3]);
        EventSubHandle h10 = event_bus_subscribe(EventType::ZCL_RAW, [](const Event&) {});
        CHECK(h10 != EVENT_SUB_INVALID, "freed slot is reusable");
        for (int i = 0; i < 8; i++)
            if (i != 3) event_bus_unsubscribe(hs[i]);
        event_bus_unsubscribe(h10);
    }

    // ── 7. queue allocation failure → EVENT_SUB_INVALID (fix 4) ───────────
    {
        stub_queue_fail_next_create();
        EventSubHandle h = event_bus_subscribe(EventType::MQTT_MSG, [](const Event&) {});
        CHECK(h == EVENT_SUB_INVALID, "queue alloc failure fails the subscribe");
        // and the slot was not half-claimed:
        int calls = 0;
        EventSubHandle h2 = event_bus_subscribe(
            EventType::MQTT_MSG, [&](const Event&) { calls++; });
        event_bus_publish(make_event(EventType::MQTT_MSG, 0x07));
        CHECK(h2 != EVENT_SUB_INVALID && event_bus_drain_handle(h2, 0) == 1 && calls == 1,
              "bus fully functional after an alloc-failed subscribe");
        event_bus_unsubscribe(h2);
    }

    // ── 8. re-init is a guarded no-op (fix 6) ──────────────────────────────
    {
        int calls = 0;
        EventSubHandle h = event_bus_subscribe(
            EventType::RULE_EVENT, [&](const Event&) { calls++; });
        event_bus_init();   // must NOT wipe the table / leak the live queue
        event_bus_publish(make_event(EventType::RULE_EVENT, 0x09));
        CHECK(event_bus_drain_handle(h, 0) == 1 && calls == 1,
              "subscription survives a second event_bus_init");
        event_bus_unsubscribe(h);
    }

    // ── 9. deprecated by-type drain still serves the main-loop pattern ────
    {
        int a = 0, b = 0;
        EventSubHandle ha = event_bus_subscribe(
            EventType::RULE_TIMER_FIRE, [&](const Event&) { a++; });
        EventSubHandle hb = event_bus_subscribe(
            EventType::RULE_TIMER_FIRE, [&](const Event&) { b++; });
        event_bus_publish(make_event(EventType::RULE_TIMER_FIRE, 0x01));
        uint8_t n = event_bus_drain(EventType::RULE_TIMER_FIRE, 0);
        CHECK(n == 2 && a == 1 && b == 1,
              "event_bus_drain(type) drains every subscriber of the type");
        event_bus_unsubscribe(ha);
        event_bus_unsubscribe(hb);
    }

    // ── 10. invalid arguments are inert ────────────────────────────────────
    {
        CHECK(event_bus_subscribe((EventType)0, [](const Event&) {}) == EVENT_SUB_INVALID,
              "type 0 subscribe rejected");
        CHECK(event_bus_subscribe(EventType::_COUNT, [](const Event&) {}) == EVENT_SUB_INVALID,
              "sentinel-type subscribe rejected");
        CHECK(event_bus_drain_handle(EVENT_SUB_INVALID, 0) == 0,
              "drain on EVENT_SUB_INVALID returns 0");
        event_bus_unsubscribe(EVENT_SUB_INVALID);   // must not crash
        Event ev{};   // type 0
        event_bus_publish(ev);                      // must not crash
        CHECK(true, "invalid unsubscribe/publish are no-ops");
    }

    printf("%s (%d failure%s)\n", s_failures ? "FAILED" : "OK",
           s_failures, s_failures == 1 ? "" : "s");
    return s_failures ? 1 : 0;
}
