// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "zhc_adapter_fallback.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "zhc/runtime/definition.hpp"
#include "zhc/types.hpp"

#include "definitions/_generic/_shared.hpp"

#include "zhc_adapter_internal.hpp"

namespace {

static const char* TAG = "zhc_fallback";

// Fixed pool — one synth slot per device. 16 fallback devices should
// be plenty; past that we recycle the oldest.
constexpr std::size_t kPoolSize     = 16;
constexpr std::size_t kMaxEndpoints = 4;
constexpr std::size_t kMaxClusters  = 12;   // matches ZAP_CLUSTERS_PER_EP
constexpr std::size_t kMaxExposes   = 16;
constexpr std::size_t kMaxFz        = 8;
constexpr std::size_t kMaxTz        = 4;
constexpr std::size_t kMaxBindings  = 8;
constexpr std::size_t kMaxReports   = 16;
constexpr std::size_t kMaxSteps     = 16;
constexpr std::size_t kScratchCap   = 64;

struct EndpointInfo {
    std::uint8_t  endpoint;
    std::uint16_t profile_id;
    std::uint16_t device_id;
    std::uint8_t  n_in;
    std::uint8_t  n_out;
    std::uint16_t in[kMaxClusters];
    std::uint16_t out[kMaxClusters];
};

// Static storage for the enum values referenced by synth exposes.
// `color_mode` is the only enum expose in the current fallback set.
constexpr const char* kColorModeEnum[] = { "hs", "xy", "color_temp" };

// One fully-stitched synthetic definition: the PreparedDefinition plus
// every array it points into. Self-contained — `def`'s pointers only
// ever reference this struct's own members, so a published `&def`
// stays valid for exactly as long as these bytes are not rewritten.
struct BuiltDef {
    zhc::PreparedDefinition def;
    zhc::Expose             exposes[kMaxExposes];
    std::size_t             n_exposes;
    const zhc::FzConverter* fz[kMaxFz];
    std::size_t             n_fz;
    const zhc::TzConverter* tz[kMaxTz];
    std::size_t             n_tz;
    zhc::BindingSpec        bindings[kMaxBindings];
    std::size_t             n_bindings;
    zhc::ReportingSpec      reports[kMaxReports];
    std::size_t             n_reports;
    zhc::ConfigStep         steps[kMaxSteps];
    std::size_t             n_steps;
    std::uint8_t            scratch[kScratchCap];
    std::size_t             n_scratch;
    char                    model[40];
    char                    manufacturer[40];
};

struct FallbackSlot {
    std::uint64_t ieee;
    std::uint32_t last_used_ms;
    EndpointInfo  eps[kMaxEndpoints];
    bool          built;
    // Base definition the active half was built against (registry defs
    // are pointer-stable, so pointer equality is a valid "same base"
    // test). Together with `built` this gates the rebuild fast path.
    const zhc::PreparedDefinition* last_base;
    // A/B double buffer. `bufs[active]` is the published half — its
    // address may be held by readers (IeeeSlot.cached_def, an in-flight
    // dispatch on the radio task, an httpd exposes walk). A rebuild
    // writes ONLY the inactive half, then publishes by flipping
    // `active` under the pool mutex; the previously-published half's
    // bytes stay untouched until the NEXT rebuild of this same slot.
    // Readers therefore never observe a half-written definition — the
    // residual rule is the one-generation lifetime documented on
    // `synth_definition()` in the header.
    std::uint8_t  active;
    BuiltDef      bufs[2];
};

FallbackSlot g_pool[kPoolSize] = {};

// Pool mutex — serializes every accessor of g_pool (alloc / evict /
// rebuild / lookup). Created from a global constructor so it exists
// before any task can call into this TU (lazy create would itself be a
// race). Same pattern as `g_cfg_addr_mtx` in zhc_adapter.cpp. The
// radio-task per-frame hot path never enters this TU (it dispatches
// through IeeeSlot.cached_def), so this lock is off the per-frame
// critical path by construction.
SemaphoreHandle_t g_pool_mtx = nullptr;
struct PoolMtxInit { PoolMtxInit() { g_pool_mtx = xSemaphoreCreateMutex(); } };
PoolMtxInit g_pool_mtx_init;

struct PoolLock {
    PoolLock()  { if (g_pool_mtx) xSemaphoreTake(g_pool_mtx, portMAX_DELAY); }
    ~PoolLock() { if (g_pool_mtx) xSemaphoreGive(g_pool_mtx); }
    PoolLock(const PoolLock&)            = delete;
    PoolLock& operator=(const PoolLock&) = delete;
};

std::uint32_t fallback_now_ms() {
    return static_cast<std::uint32_t>(esp_timer_get_time() / 1000ULL);
}

// Drop a slot's identity + interview data but PRESERVE bufs[] and
// `active`: a stale reader may still hold a pointer into the
// previously-published half, and the next rebuild (for whatever ieee
// owns the slot next) writes the OTHER half first — so published bytes
// survive one more generation even across a slot repurpose.
void reset_slot(FallbackSlot& s) {
    s.ieee         = 0;
    s.last_used_ms = 0;
    std::memset(s.eps, 0, sizeof(s.eps));
    s.built     = false;
    s.last_base = nullptr;
}

// Tell zhc_adapter to forget any IeeeSlot.cached_def /
// .cached_supplement pointing into this slot's built-def storage
// (both A/B halves) — must run BEFORE the slot is repurposed so the
// old device's frames can't decode through the new device's def.
void invalidate_adapter_cache_for(FallbackSlot& s) {
    zhc_adapter_internal::invalidate_cached_defs_in(
        &s.bufs[0], &s.bufs[0] + 2);
}

// Linear scans are fine at pool size 16.
FallbackSlot* find_slot(std::uint64_t ieee) {
    if (ieee == 0) return nullptr;
    for (auto& s : g_pool) if (s.ieee == ieee) return &s;
    return nullptr;
}

FallbackSlot* alloc_slot(std::uint64_t ieee) {
    if (auto* s = find_slot(ieee)) return s;
    const std::uint32_t now = fallback_now_ms();
    // Prefer an empty slot.
    for (auto& s : g_pool) {
        if (s.ieee == 0) {
            reset_slot(s);
            s.ieee = ieee;
            s.last_used_ms = now;
            return &s;
        }
    }
    // LRU eviction — `last_used_ms` is stamped on every
    // register_endpoint / synth_definition touch, so the victim really
    // is the least-recently-used device, not just slot 0.
    FallbackSlot* victim = &g_pool[0];
    for (auto& s : g_pool) if (s.last_used_ms < victim->last_used_ms) victim = &s;
    ESP_LOGW(TAG, "pool full — evicting ieee=0x%016llx",
              static_cast<unsigned long long>(victim->ieee));
    invalidate_adapter_cache_for(*victim);
    reset_slot(*victim);
    victim->ieee = ieee;
    victim->last_used_ms = now;
    return victim;
}

std::uint8_t first_endpoint_with_cluster(const FallbackSlot& s,
                                          std::uint16_t cluster) {
    for (const auto& ep : s.eps) {
        if (ep.endpoint == 0) continue;
        for (std::uint8_t i = 0; i < ep.n_in; ++i) {
            if (ep.in[i] == cluster) return ep.endpoint;
        }
    }
    return 0;
}

// If a base definition already binds this cluster on any endpoint, skip
// it so the supplementary synth doesn't duplicate exposes / bindings /
// reports / reads the registry already drives.
bool base_handles_cluster(const zhc::PreparedDefinition* base,
                           std::uint16_t cluster) {
    if (!base) return false;
    for (std::uint8_t i = 0; i < base->bindings_count; ++i) {
        if (base->bindings[i].cluster_id == cluster) return true;
    }
    return false;
}

bool fz_already_in(BuiltDef& b, const zhc::FzConverter* fz) {
    for (std::size_t i = 0; i < b.n_fz; ++i) if (b.fz[i] == fz) return true;
    return false;
}

void push_fz(BuiltDef& b, const zhc::FzConverter* fz) {
    if (!fz || fz_already_in(b, fz) || b.n_fz >= kMaxFz) return;
    b.fz[b.n_fz++] = fz;
}

void push_tz(BuiltDef& b, const zhc::TzConverter* tz) {
    if (!tz || b.n_tz >= kMaxTz) return;
    for (std::size_t i = 0; i < b.n_tz; ++i) if (b.tz[i] == tz) return;
    b.tz[b.n_tz++] = tz;
}

void push_expose(BuiltDef& b, const zhc::Expose& e) {
    if (b.n_exposes >= kMaxExposes) return;
    b.exposes[b.n_exposes++] = e;
}

void push_binding(BuiltDef& b, std::uint8_t ep, std::uint16_t cluster) {
    if (b.n_bindings >= kMaxBindings) return;
    b.bindings[b.n_bindings++] = { ep, cluster };
}

void push_report(BuiltDef& b, std::uint8_t ep, std::uint16_t cluster,
                  std::uint16_t attr, std::uint8_t type,
                  std::uint16_t mn, std::uint16_t mx, std::uint32_t change) {
    if (b.n_reports >= kMaxReports) return;
    b.reports[b.n_reports++] = { ep, cluster, attr, type, mn, mx, change, 0 };
}

const std::uint8_t* push_read_attrs(BuiltDef& b,
                                     const std::uint16_t* attrs,
                                     std::size_t count) {
    // Encode `count` attr ids LE into scratch; return pointer into scratch.
    const std::size_t need = count * 2;
    if (b.n_scratch + need > kScratchCap) return nullptr;
    std::uint8_t* p = b.scratch + b.n_scratch;
    for (std::size_t i = 0; i < count; ++i) {
        p[2 * i + 0] = static_cast<std::uint8_t>(attrs[i] & 0xFF);
        p[2 * i + 1] = static_cast<std::uint8_t>((attrs[i] >> 8) & 0xFF);
    }
    b.n_scratch += need;
    return p;
}

void push_read_step(BuiltDef& b, std::uint8_t ep, std::uint16_t cluster,
                     const std::uint16_t* attrs, std::size_t count) {
    if (b.n_steps >= kMaxSteps) return;
    const std::uint8_t* payload = push_read_attrs(b, attrs, count);
    if (!payload) return;
    zhc::ConfigStep step{};
    step.op           = zhc::ConfigStepOp::Read;
    step.endpoint     = ep;
    step.cluster_id   = cluster;
    step.cmd_id       = 0;
    step.flags        = 0;
    step.payload      = payload;
    step.payload_len  = static_cast<std::uint8_t>(count * 2);
    step.wait_ms      = 1500;
    b.steps[b.n_steps++] = step;
}

// ─── Cluster emitters ──────────────────────────────────────────────
//
// Each emitter fires only if the device actually has the cluster.
// Reuses the library's generic converters (no new FZ/TZ code needed).
// `s` supplies the interview data (endpoints / clusters); `b` is the
// build target — always the slot's INACTIVE half, never published.

void emit_on_off(const FallbackSlot& s, BuiltDef& b,
                  const zhc::PreparedDefinition* base) {
    if (base_handles_cluster(base, 0x0006)) return;
    const std::uint8_t ep = first_endpoint_with_cluster(s, 0x0006);
    if (!ep) return;
    push_expose(b, { "state", zhc::ExposeType::Binary, zhc::Access::StateSet,
                       nullptr, nullptr, nullptr, 0,
                       zhc::ExposeCategory::State });
    push_fz(b, &zhc::generic::kFzOnOff);
    push_tz(b, &zhc::generic::kTzOnOff);
    push_binding(b, ep, 0x0006);
    push_report(b, ep, 0x0006, 0x0000, /*bool*/ 0x10, 0, 3600, 1);
    static constexpr std::uint16_t kReadOnOff[] = { 0x0000 };
    push_read_step(b, ep, 0x0006, kReadOnOff, 1);
}

void emit_level_ctrl(const FallbackSlot& s, BuiltDef& b,
                      const zhc::PreparedDefinition* base) {
    if (base_handles_cluster(base, 0x0008)) return;
    const std::uint8_t ep = first_endpoint_with_cluster(s, 0x0008);
    if (!ep) return;
    push_expose(b, { "brightness", zhc::ExposeType::Numeric,
                       zhc::Access::StateSet, nullptr, nullptr, nullptr, 0,
                       zhc::ExposeCategory::State });
    push_fz(b, &zhc::generic::kFzBrightness);
    push_tz(b, &zhc::generic::kTzBrightness);
    push_binding(b, ep, 0x0008);
    push_report(b, ep, 0x0008, 0x0000, /*u8*/ 0x20, 5, 3600, 1);
    static constexpr std::uint16_t kReadLevel[] = { 0x0000 };
    push_read_step(b, ep, 0x0008, kReadLevel, 1);
}

void emit_color_ctrl(const FallbackSlot& s, BuiltDef& b,
                      const zhc::PreparedDefinition* base) {
    if (base_handles_cluster(base, 0x0300)) return;
    const std::uint8_t ep = first_endpoint_with_cluster(s, 0x0300);
    if (!ep) return;
    push_expose(b, { "color_temp", zhc::ExposeType::Numeric,
                       zhc::Access::StateSet, "mired", nullptr, nullptr, 0,
                       zhc::ExposeCategory::State });
    push_expose(b, { "color_x",    zhc::ExposeType::Numeric,
                       zhc::Access::State, nullptr, nullptr, nullptr, 0,
                       zhc::ExposeCategory::State });
    push_expose(b, { "color_y",    zhc::ExposeType::Numeric,
                       zhc::Access::State, nullptr, nullptr, nullptr, 0,
                       zhc::ExposeCategory::State });
    push_expose(b, { "color_mode", zhc::ExposeType::Enum,
                       zhc::Access::State, nullptr, nullptr,
                       kColorModeEnum,
                       sizeof(kColorModeEnum) / sizeof(kColorModeEnum[0]),
                       zhc::ExposeCategory::State });
    push_fz(b, &zhc::generic::kFzColorTemperature);
    push_fz(b, &zhc::generic::kFzColor);
    push_tz(b, &zhc::generic::kTzColorTemp);
    push_binding(b, ep, 0x0300);
    push_report(b, ep, 0x0300, 0x0007, /*u16*/ 0x21, 5, 3600, 1);
    static constexpr std::uint16_t kReadColor[] = { 0x0003, 0x0004, 0x0007, 0x0008 };
    push_read_step(b, ep, 0x0300, kReadColor, 4);
}

void emit_power_cfg(const FallbackSlot& s, BuiltDef& b,
                     const zhc::PreparedDefinition* base) {
    if (base_handles_cluster(base, 0x0001)) return;
    const std::uint8_t ep = first_endpoint_with_cluster(s, 0x0001);
    if (!ep) return;
    push_expose(b, { "voltage", zhc::ExposeType::Numeric,
                       zhc::Access::State, "mV", nullptr, nullptr, 0,
                       zhc::ExposeCategory::Diagnostic });
    push_expose(b, { "battery", zhc::ExposeType::Numeric,
                       zhc::Access::State, "%", nullptr, nullptr, 0,
                       zhc::ExposeCategory::Diagnostic });
    push_fz(b, &zhc::generic::kFzBattery);
    push_binding(b, ep, 0x0001);
    push_report(b, ep, 0x0001, 0x0020, /*u8*/ 0x20, 3600, 65535, 1);
    push_report(b, ep, 0x0001, 0x0021, /*u8*/ 0x20, 3600, 65535, 1);
    static constexpr std::uint16_t kReadBatt[] = { 0x0020, 0x0021 };
    push_read_step(b, ep, 0x0001, kReadBatt, 2);
}

void emit_temp_meas(const FallbackSlot& s, BuiltDef& b,
                     const zhc::PreparedDefinition* base) {
    if (base_handles_cluster(base, 0x0402)) return;
    const std::uint8_t ep = first_endpoint_with_cluster(s, 0x0402);
    if (!ep) return;
    push_expose(b, { "temperature", zhc::ExposeType::Numeric,
                       zhc::Access::State, "°C", nullptr, nullptr, 0,
                       zhc::ExposeCategory::State });
    push_fz(b, &zhc::generic::kFzTemperature);
    push_binding(b, ep, 0x0402);
    push_report(b, ep, 0x0402, 0x0000, /*i16*/ 0x29, 30, 3600, 100);
    static constexpr std::uint16_t kReadT[] = { 0x0000 };
    push_read_step(b, ep, 0x0402, kReadT, 1);
}

void emit_hum_meas(const FallbackSlot& s, BuiltDef& b,
                    const zhc::PreparedDefinition* base) {
    if (base_handles_cluster(base, 0x0405)) return;
    const std::uint8_t ep = first_endpoint_with_cluster(s, 0x0405);
    if (!ep) return;
    push_expose(b, { "humidity", zhc::ExposeType::Numeric,
                       zhc::Access::State, "%", nullptr, nullptr, 0,
                       zhc::ExposeCategory::State });
    push_fz(b, &zhc::generic::kFzHumidity);
    push_binding(b, ep, 0x0405);
    push_report(b, ep, 0x0405, 0x0000, /*u16*/ 0x21, 30, 3600, 100);
    static constexpr std::uint16_t kReadH[] = { 0x0000 };
    push_read_step(b, ep, 0x0405, kReadH, 1);
}

void emit_pressure_meas(const FallbackSlot& s, BuiltDef& b,
                         const zhc::PreparedDefinition* base) {
    if (base_handles_cluster(base, 0x0403)) return;
    const std::uint8_t ep = first_endpoint_with_cluster(s, 0x0403);
    if (!ep) return;
    push_expose(b, { "pressure", zhc::ExposeType::Numeric,
                       zhc::Access::State, "hPa", nullptr, nullptr, 0,
                       zhc::ExposeCategory::State });
    push_fz(b, &zhc::generic::kFzPressure);
    push_binding(b, ep, 0x0403);
    push_report(b, ep, 0x0403, 0x0000, /*i16*/ 0x29, 30, 3600, 100);
    static constexpr std::uint16_t kReadP[] = { 0x0000 };
    push_read_step(b, ep, 0x0403, kReadP, 1);
}

void emit_illuminance(const FallbackSlot& s, BuiltDef& b,
                       const zhc::PreparedDefinition* base) {
    if (base_handles_cluster(base, 0x0400)) return;
    const std::uint8_t ep = first_endpoint_with_cluster(s, 0x0400);
    if (!ep) return;
    push_expose(b, { "illuminance", zhc::ExposeType::Numeric,
                       zhc::Access::State, "lux", nullptr, nullptr, 0,
                       zhc::ExposeCategory::State });
    push_fz(b, &zhc::generic::kFzIlluminance);
    push_binding(b, ep, 0x0400);
    push_report(b, ep, 0x0400, 0x0000, /*u16*/ 0x21, 30, 3600, 100);
    static constexpr std::uint16_t kReadL[] = { 0x0000 };
    push_read_step(b, ep, 0x0400, kReadL, 1);
}

// Build a complete synthetic definition into `b` — which MUST be the
// slot's inactive half. Never writes the slot itself; the caller
// publishes by flipping `s.active` afterwards.
void rebuild(const FallbackSlot& s, BuiltDef& b,
              const char* model, const char* manufacturer,
              const zhc::PreparedDefinition* base) {
    b.n_exposes = 0;
    b.n_fz = 0;
    b.n_tz = 0;
    b.n_bindings = 0;
    b.n_reports = 0;
    b.n_steps = 0;
    b.n_scratch = 0;

    emit_on_off(s, b, base);
    emit_level_ctrl(s, b, base);
    emit_color_ctrl(s, b, base);
    emit_power_cfg(s, b, base);
    emit_temp_meas(s, b, base);
    emit_hum_meas(s, b, base);
    emit_pressure_meas(s, b, base);
    emit_illuminance(s, b, base);

    // Stash the model / manufacturer so registry-style log messages
    // don't show "-/-" for generic devices.
    std::snprintf(b.model,        sizeof(b.model),
                   "%s", model        ? model        : "generic");
    std::snprintf(b.manufacturer, sizeof(b.manufacturer),
                   "%s", manufacturer ? manufacturer : "generic");

    // Stitch the PreparedDefinition. Most fields stay null — we don't
    // touch meta, white_labels, on_event, etc. from a fallback.
    b.def = zhc::PreparedDefinition{};
    b.def.zigbee_models        = nullptr;
    b.def.zigbee_models_count  = 0;
    b.def.manufacturer_names   = nullptr;
    b.def.manufacturer_names_count = 0;
    b.def.manufacturer_name_prefix = nullptr;
    b.def.model                = b.model;
    b.def.vendor               = b.manufacturer;
    b.def.exposes              = b.exposes;
    b.def.exposes_count        = static_cast<std::uint8_t>(b.n_exposes);
    b.def.from_zigbee          = b.fz;
    b.def.from_zigbee_count    = static_cast<std::uint8_t>(b.n_fz);
    b.def.to_zigbee            = b.tz;
    b.def.to_zigbee_count      = static_cast<std::uint8_t>(b.n_tz);
    b.def.bindings             = b.bindings;
    b.def.bindings_count       = static_cast<std::uint8_t>(b.n_bindings);
    b.def.reports              = b.reports;
    b.def.reports_count        = static_cast<std::uint8_t>(b.n_reports);
    b.def.config_steps         = b.steps;
    b.def.config_steps_count   = static_cast<std::uint8_t>(b.n_steps);

    const bool any = b.n_exposes || b.n_fz || b.n_tz ||
                     b.n_bindings || b.n_reports || b.n_steps;
    if (any) {
        ESP_LOGI(TAG,
                  "[fallback] synth def ieee=0x%016llx exposes=%zu fz=%zu tz=%zu "
                  "bindings=%zu reports=%zu steps=%zu",
                  static_cast<unsigned long long>(s.ieee),
                  b.n_exposes, b.n_fz, b.n_tz,
                  b.n_bindings, b.n_reports, b.n_steps);
    } else {
        ESP_LOGD(TAG,
                  "[fallback] synth def ieee=0x%016llx exposes=0 fz=0 tz=0 "
                  "bindings=0 reports=0 steps=0 (empty)",
                  static_cast<unsigned long long>(s.ieee));
    }
}

}  // namespace

namespace zhc_fallback {

void register_endpoint(std::uint64_t ieee,
                       std::uint8_t  endpoint,
                       std::uint16_t profile_id,
                       std::uint16_t device_id,
                       const std::uint16_t* in_clusters,  std::size_t n_in,
                       const std::uint16_t* out_clusters, std::size_t n_out) {
    if (ieee == 0 || endpoint == 0) return;

    // Skip the Green Power endpoint (0xF2) — it doesn't carry standard
    // clusters we map.
    if (endpoint == 0xF2) return;

    PoolLock lock;
    FallbackSlot* s = alloc_slot(ieee);
    if (!s) return;
    s->last_used_ms = fallback_now_ms();

    // Find existing endpoint slot or a free one.
    EndpointInfo* target = nullptr;
    for (auto& ep : s->eps) {
        if (ep.endpoint == endpoint) { target = &ep; break; }
    }
    if (!target) {
        for (auto& ep : s->eps) {
            if (ep.endpoint == 0) { target = &ep; break; }
        }
    }
    if (!target) return;   // too many endpoints — drop extras

    target->endpoint   = endpoint;
    target->profile_id = profile_id;
    target->device_id  = device_id;
    target->n_in       = 0;
    target->n_out      = 0;
    const std::size_t cap_in  = std::min(n_in,  kMaxClusters);
    const std::size_t cap_out = std::min(n_out, kMaxClusters);
    for (std::size_t i = 0; i < cap_in;  ++i) target->in[target->n_in++]   = in_clusters[i];
    for (std::size_t i = 0; i < cap_out; ++i) target->out[target->n_out++] = out_clusters[i];

    // Force rebuild on next synth_definition() call. The published
    // half stays byte-intact — readers that already resolved it keep
    // decoding against the pre-update view until the rebuild swaps.
    s->built = false;
}

void clear(std::uint64_t ieee) {
    PoolLock lock;
    if (FallbackSlot* s = find_slot(ieee)) {
        // Same stale-pointer discipline as eviction: drop any adapter
        // cache entries into this slot, then reset identity/interview
        // data while leaving the published bytes intact for one more
        // generation (an in-flight dispatch may still be reading them).
        invalidate_adapter_cache_for(*s);
        reset_slot(*s);
    }
}

bool has_data(std::uint64_t ieee) {
    PoolLock lock;
    const FallbackSlot* s = find_slot(ieee);
    if (!s) return false;
    for (const auto& ep : s->eps) if (ep.endpoint != 0) return true;
    return false;
}

const zhc::PreparedDefinition* synth_definition(std::uint64_t ieee,
                                                 const char* model,
                                                 const char* manufacturer,
                                                 const zhc::PreparedDefinition* base) {
    PoolLock lock;
    FallbackSlot* s = find_slot(ieee);
    if (!s) return nullptr;
    s->last_used_ms = fallback_now_ms();

    // Fast path: already built for this base AND the identity labels
    // baked into the def still match — return the published half
    // untouched. Rebuilds happen only on new interview data
    // (register_endpoint → built=false), a base flip (unmatched →
    // registry-matched supplement), or a late identity fill changing
    // the model/manufacturer strings.
    const char* eff_model = model        ? model        : "generic";
    const char* eff_manu  = manufacturer ? manufacturer : "generic";
    const BuiltDef* cur = &s->bufs[s->active];
    const bool fresh =
        s->built && s->last_base == base &&
        std::strncmp(cur->model,        eff_model, sizeof(cur->model) - 1)        == 0 &&
        std::strncmp(cur->manufacturer, eff_manu,  sizeof(cur->manufacturer) - 1) == 0;
    if (!fresh) {
        // Build into the INACTIVE half, publish by flipping `active`.
        // Never rebuild a published def in place: readers holding the
        // old pointer (cached_def fast path, in-flight dispatch) keep
        // seeing a fully-consistent definition.
        BuiltDef& spare = s->bufs[s->active ^ 1];
        rebuild(*s, spare, model, manufacturer, base);
        s->active   = static_cast<std::uint8_t>(s->active ^ 1);
        s->last_base = base;
        s->built     = true;
        cur = &s->bufs[s->active];
    }

    if (cur->n_exposes == 0) {
        // No mapped clusters left after subtracting `base`'s coverage
        // — either a pure router, unsupported device type, or the
        // registry def already handles everything.
        return nullptr;
    }
    return &cur->def;
}

}  // namespace zhc_fallback
