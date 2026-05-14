// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// task_stacks.h — single source of truth for FreeRTOS task stack sizes
// across both ESP32-S3 and ESP32-P4 firmwares. Edit values here only.
// Stack monitors iterate `kTable` to compute used% from the runtime
// uxTaskGetStackHighWaterMark watermark.
#pragma once
#include <cstdint>
#include "sdkconfig.h"   // CONFIG_IDF_TARGET_ESP32P4 / ESP32S3

namespace zhac::stack {

// ── P4 main tasks ──────────────────────────────────────────────────
inline constexpr uint32_t kZigbee       = 6144;
inline constexpr uint32_t kHapP4        = 6144;
inline constexpr uint32_t kEventBus     = 8192;
inline constexpr uint32_t kLog          = 6144;
inline constexpr uint32_t kWdt          = 2048;
inline constexpr uint32_t kStackMonP4   = 2048;
inline constexpr uint32_t kButtons      = 2048;
inline constexpr uint32_t kHapBench     = 4096;

// ── P4 component tasks ─────────────────────────────────────────────
inline constexpr uint32_t kHapSlave     = 8192;
inline constexpr uint32_t kRuleFlush    = 6144;   // NVS commit + ESP_LOGI
inline constexpr uint32_t kZapFlush     = 6144;   // ditto — observed free=136B at 3072
inline constexpr uint32_t kZbIdentity   = 4096;
inline constexpr uint32_t kZbConfigure  = 6144;
inline constexpr uint32_t kZbInterview  = 8192;
inline constexpr uint32_t kZclAttr      = 8192;
inline constexpr uint32_t kZnpWorker    = 4096;
inline constexpr uint32_t kZnpRx        = 4096;
// rule_cron: the per-second cron tick path can chain into
// execute_rule → on_script_run → lua_scheduler_push_run_named, which
// stack-allocates a full LuaMsg (~256 B union of payload/named).
// 4 KB was tight even at minute resolution; per-second tripped the
// stack-protection canary. 8 KB gives ~2× margin.
inline constexpr uint32_t kRuleCron     = 8192;
inline constexpr uint32_t kDeviceShadow = 4096;
inline constexpr uint32_t kTaskLua      = 8192;
inline constexpr uint32_t kMqttPubP4    = 4096;
inline constexpr uint32_t kInterviewP4  = 8192;   // alias of kZbInterview when used from main

// ── S3 main tasks ──────────────────────────────────────────────────
// TaskWiFi is created internally by ESP-IDF's wifi stack, not by us.
// Declared here only so the stack monitor can report on it.
inline constexpr uint32_t kWifi         = 4096;
inline constexpr uint32_t kHapS3        = 8192;
inline constexpr uint32_t kHttp         = 8192;
inline constexpr uint32_t kTimeSync     = 3072;
inline constexpr uint32_t kOta          = 8192;
inline constexpr uint32_t kP4Ota        = 8192;
inline constexpr uint32_t kStackMonS3   = 3072;
inline constexpr uint32_t kAlertPersist = 3072;
inline constexpr uint32_t kMqttPubS3    = 6144;
inline constexpr uint32_t kTgWorker     = 16384;  // TLS handshake to api.telegram.org
                                                  // needs ~10 KB for mbedtls SHA + x509.

// Task-name → size map for stack-monitor iteration. nullptr name
// terminates. Some names appear on both chips (TaskHAP, TaskStackMon)
// — at runtime, the lookup resolves to whichever entry matches the
// running task on this binary; both values are the same kind of upper
// bound so collisions don't mislead.
struct Entry { const char* name; uint32_t size; };

// Chip-conditional entries. Several names collide between chips
// (TaskHAP, TaskStackMon, mqtt_pub) with different sizes; each binary
// must see only its own entries or the reporter would show the wrong
// total and produce negative used values from unsigned wrap.
inline constexpr Entry kTable[] = {
#if CONFIG_IDF_TARGET_ESP32P4
    // P4 main
    {"TaskZigbee",    kZigbee},
    {"TaskHAP",       kHapP4},
    {"TaskEventBus",  kEventBus},
    {"TaskLog",       kLog},
    {"TaskWDT",       kWdt},
    {"TaskStackMon",  kStackMonP4},
    {"TaskButtons",   kButtons},
    {"hap_bench",     kHapBench},
    // P4 components
    {"hap_slave",     kHapSlave},
    {"rule_flush",    kRuleFlush},
    {"zap_flush",     kZapFlush},
    {"zb_identity",   kZbIdentity},
    {"zb_configure",  kZbConfigure},
    {"zb_interview",  kZbInterview},
    {"zcl_attr",      kZclAttr},
    {"znp_worker",    kZnpWorker},
    {"znp_rx",        kZnpRx},
    {"rule_cron",     kRuleCron},
    {"task_shadow",   kDeviceShadow},
    {"TaskLua",       kTaskLua},
    {"mqtt_pub",      kMqttPubP4},
#elif CONFIG_IDF_TARGET_ESP32S3
    // S3 main
    {"TaskWiFi",      kWifi},        // ESP-IDF internal; for reporting only
    {"TaskHAP",       kHapS3},
    {"TaskHTTP",      kHttp},
    {"TaskTimeSync",  kTimeSync},
    {"TaskOTA",       kOta},
    {"TaskP4OTA",     kP4Ota},
    {"TaskStackMon",  kStackMonS3},
    {"TaskAlertPrst", kAlertPersist},
    {"mqtt_pub",      kMqttPubS3},
    {"TgWorker",      kTgWorker},
#endif
    {nullptr, 0},
};

inline uint32_t task_stack_size_for(const char* name) {
    if (!name) return 0;
    for (const Entry* e = kTable; e->name != nullptr; ++e) {
        const char* a = name;
        const char* b = e->name;
        while (*a && *a == *b) { ++a; ++b; }
        if (*a == 0 && *b == 0) return e->size;
    }
    return 0;
}

}  // namespace zhac::stack
