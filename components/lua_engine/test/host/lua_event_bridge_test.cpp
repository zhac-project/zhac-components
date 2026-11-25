// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host test of the Lua event bridge's ATTR_CHANGE subscription (M48, 2026-11-25).
//
// The bridge subscribes at ~4.0 s into the boot, TaskEventBus starts draining at ~6.25 s, and ~20
// attribute reports queue in between. At the default depth of 16 the oldest of 30 were overwritten.
// The real bridge TU and the real event_bus run here; lua_scheduler_push_event is a counter.
#include "event_bus.h"
#include "esp_log.h"          // host shim: stub_log_count
#include "lua_scheduler.h"

#include <cstdio>

extern "C" void lua_engine_event_bridge_start(void);

static int s_attr_pushed = 0;

extern "C" bool lua_scheduler_push_event(uint8_t event_kind, const void* payload_96) {
    (void)payload_96;
    if (event_kind == 1) s_attr_pushed++;   // LUA_EVT_ATTR
    return true;
}

static int s_failures = 0;
#define CHECK(cond, msg) do {                                         \
    if (cond) { printf("PASS: %s\n", msg); }                          \
    else      { printf("FAIL: %s\n", msg); s_failures++; }            \
} while (0)

int main() {
    event_bus_init();
    lua_engine_event_bridge_start();

    Event ev{};
    ev.type = EventType::ATTR_CHANGE;
    for (int i = 0; i < 30; i++) event_bus_publish(ev);   // nothing drains yet
    for (int i = 0; i < 8; i++) if (event_bus_drain(EventType::ATTR_CHANGE, 0) == 0) break;

    printf("bridge: pushed=%d of 30\n", s_attr_pushed);
    CHECK(s_attr_pushed == 30, "30 attribute events queued before the first drain all reach Lua");
    CHECK(stub_log_count('W', "queue full") == 0, "and the bus logs no overflow");

    printf("%s (%d failure%s)\n", s_failures ? "FAILED" : "OK", s_failures, s_failures == 1 ? "" : "s");
    return s_failures ? 1 : 0;
}
