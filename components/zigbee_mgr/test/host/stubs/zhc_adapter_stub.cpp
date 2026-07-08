// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host stub for the zhc_adapter (embedded-zhc <-> platform) boundary. The real
// adapter pulls in the entire zhc device-definition library; the zigbee_mgr
// harness only needs the SYMBOLS to resolve and a couple of observable hooks.
// Every function is a deterministic no-op EXCEPT zhac_adapter_invalidate_def_
// cache(), which records its argument so the test can assert that
// zigbee_pool_remove() drops the adapter's cached definition.
//
// Signatures come from the real zhc_adapter.h (extern "C"); definitions here
// inherit that linkage. Parameters are left unnamed so the stub stays clean
// under -Wall -Wextra -Werror (no production code is modified).
#include "zhc_adapter.h"
#include "znp_stub.h"   // spy accessor declarations

namespace {
uint64_t g_last_invalidated_ieee = 0;
int      g_invalidate_count      = 0;
}  // namespace

// ── cache-invalidation spy ──────────────────────────────────────────────
uint64_t zhac_stub_last_invalidated_ieee() { return g_last_invalidated_ieee; }
int      zhac_stub_invalidate_count()       { return g_invalidate_count; }
void     zhac_stub_reset() { g_last_invalidated_ieee = 0; g_invalidate_count = 0; }

// ── lifecycle / registration hooks (no-ops) ─────────────────────────────
void zhac_adapter_init(void) {}
void zhac_adapter_register_send(zhac_af_send_fn_t) {}
void zhac_adapter_register_shadow(zhac_shadow_update_fn_t) {}
void zhac_adapter_register_configure(zhac_configure_bind_fn_t,
                                     zhac_configure_report_fn_t) {}
void zhac_adapter_register_configure_ex(zhac_configure_cmd_fn_t,
                                        zhac_configure_read_fn_t,
                                        zhac_configure_sleep_fn_t) {}
void zhac_adapter_register_configure_write(zhac_configure_write_fn_t) {}
void zhac_adapter_set_runtime_addr(uint64_t, uint16_t) {}
void zhac_adapter_fallback_clear(uint64_t) {}
void zhac_adapter_register_endpoint(uint64_t, uint8_t, uint16_t, uint16_t,
                                    const uint16_t*, size_t,
                                    const uint16_t*, size_t) {}

void zhac_adapter_invalidate_def_cache(uint64_t ieee) {
    g_last_invalidated_ieee = ieee;
    g_invalidate_count++;
}

// ── query / decode / configure / send (deterministic stubs) ─────────────
bool zhac_adapter_has_def(uint64_t, const char*, const char*) { return false; }

uint8_t zhac_adapter_power_source_override(const char*, const char*) { return 0; }

bool zhac_adapter_try_decode(uint64_t, const char*, const char*, uint16_t,
                             uint16_t, uint8_t, uint8_t, const uint8_t*,
                             size_t) {
    return false;
}

bool zhac_adapter_resolve_labels(const char*, const char*, char*, size_t,
                                 char*, size_t) {
    return false;
}

size_t zhac_adapter_build_exposes_json(uint64_t, const char*, const char*,
                                       char*, size_t) {
    return 0;
}

bool zhac_adapter_configure(uint64_t, uint16_t, const char*, const char*) {
    return true;
}

bool zhac_adapter_send_bool(uint64_t, const char*, const char*, uint16_t,
                            uint8_t, const char*, bool) {
    return false;
}

bool zhac_adapter_send_uint(uint64_t, const char*, const char*, uint16_t,
                            uint8_t, const char*, uint64_t) {
    return false;
}

bool zhac_adapter_send_string(uint64_t, const char*, const char*, uint16_t,
                              uint8_t, const char*, const char*) {
    return false;
}
