// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host regression test for the self-feeding rule-event loop.
//
// A rule like `ON Event#x DO event x ENDON` republishes into the same
// event_bus queue that event_bus_drain is currently draining with ticks=0,
// so without a per-payload hop TTL the drain never returns and the P4
// main loop (task_event_bus) wedges until the watchdog reboots — and the
// rule persists in NVS, so it re-wedges every boot. The old
// MAX_DISPATCH_DEPTH counter could not see this: every queued delivery is
// a fresh dispatch_event call at depth 0.
//
// The queue shim converts the would-be infinite drain into a detectable
// "runaway" flag (see stubs/freertos/queue.h) so the test fails loudly on
// regressing code instead of hanging.
#include "simple_rules.h"
#include "event_bus.h"
#include "freertos/queue.h"   // host shim instrumentation

#include <cstdio>
#include <cstring>

// Keep in sync with MAX_EVENT_HOPS in simple_rules.cpp. A chain of N hops
// delivers N+1 rule firings (the hop-0 external event plus N republishes).
static constexpr uint8_t kMaxEventHops = 8;

static int s_failures = 0;
#define CHECK(cond, msg) do {                                         \
    if (cond) { printf("PASS: %s\n", msg); }                          \
    else      { printf("FAIL: %s\n", msg); s_failures++; }            \
} while (0)

// Publish a RULE_EVENT exactly the way external producers do (REST/Lua/
// timer paths all start from a zero-initialised Event, so hop == 0).
static void publish_external_event(const char* name) {
    Event ev{};
    ev.type = EventType::RULE_EVENT;
    auto& p = *reinterpret_cast<RuleEventPayload*>(ev.data);
    strncpy(p.name, name, sizeof(p.name) - 1);
    p.name[sizeof(p.name) - 1] = '\0';
    event_bus_publish(ev);
}

// Mimic the firmware main loop (task_event_bus): drain until quiescent.
static uint32_t drain_until_quiet() {
    uint32_t total = 0;
    for (int pass = 0; pass < 32; pass++) {
        uint8_t n = event_bus_drain(EventType::RULE_EVENT, 0);
        total += n;
        if (n == 0) break;
    }
    return total;
}

int main() {
    event_bus_init();
    simple_rules_init();

    // ── Scenario 1: self-feeding rule must not wedge the drain ────────────
    uint16_t loop_id = 0;
    CHECK(simple_rules_add("loop", "ON Event#x DO event x ENDON", &loop_id),
          "install self-feeding rule");

    stub_queue_set_receive_budget(64);   // unfixed code spins past this
    publish_external_event("x");
    uint32_t fired = drain_until_quiet();
    printf("scenario 1: fired=%lu\n", (unsigned long)fired);

    CHECK(!stub_queue_runaway(),
          "drain returns (no self-feeding re-enqueue past the budget)");
    CHECK(fired >= 2, "rule event chain actually chained (>= 2 firings)");
    CHECK(fired <= (uint32_t)kMaxEventHops + 1,
          "chain cut after MAX_EVENT_HOPS hops");

    // ── Scenario 2: each external event gets a fresh hop budget ──────────
    stub_queue_set_receive_budget(64);
    publish_external_event("x");
    uint32_t fired2 = drain_until_quiet();
    printf("scenario 2: fired=%lu\n", (unsigned long)fired2);

    CHECK(!stub_queue_runaway(), "second external event also terminates");
    CHECK(fired2 >= 2 && fired2 <= (uint32_t)kMaxEventHops + 1,
          "fresh external event re-fires with a fresh budget");

    // ── Scenario 3: legitimate short chains keep working ──────────────────
    uint16_t a_id = 0, b_id = 0;
    CHECK(simple_rules_add("chain_a", "ON Event#a DO event b ENDON", &a_id),
          "install chain rule a->b");
    CHECK(simple_rules_add("chain_b", "ON Event#b DO log done ENDON", &b_id),
          "install chain rule b->log");

    stub_queue_set_receive_budget(64);
    publish_external_event("a");
    uint32_t fired3 = drain_until_quiet();
    printf("scenario 3: fired=%lu\n", (unsigned long)fired3);

    CHECK(!stub_queue_runaway(), "legitimate chain terminates");
    CHECK(fired3 == 2, "two-rule chain fires exactly twice (a then b)");

    printf("%s (%d failure%s)\n", s_failures ? "FAILED" : "OK",
           s_failures, s_failures == 1 ? "" : "s");
    return s_failures ? 1 : 0;
}
