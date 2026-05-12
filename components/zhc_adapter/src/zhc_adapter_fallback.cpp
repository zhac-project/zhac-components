// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "zhc_adapter_fallback.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "esp_log.h"

#include "zhc/runtime/definition.hpp"
#include "zhc/types.hpp"

#include "definitions/_generic/_shared.hpp"

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

struct FallbackSlot {
    std::uint64_t ieee;
    std::uint32_t last_used_ms;
    EndpointInfo  eps[kMaxEndpoints];
    bool          built;
    // PreparedDefinition and its backing arrays. All member arrays live
    // inside the slot so `def` pointers stay valid for the slot's
    // lifetime. Reset on re-register.
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

FallbackSlot g_pool[kPoolSize] = {};

// Linear scans are fine at pool size 16.
FallbackSlot* find_slot(std::uint64_t ieee) {
    if (ieee == 0) return nullptr;
    for (auto& s : g_pool) if (s.ieee == ieee) return &s;
    return nullptr;
}

FallbackSlot* alloc_slot(std::uint64_t ieee, std::uint32_t now_ms) {
    if (auto* s = find_slot(ieee)) return s;
    // Prefer an empty slot.
    for (auto& s : g_pool) if (s.ieee == 0) { s = {}; s.ieee = ieee; s.last_used_ms = now_ms; return &s; }
    // LRU eviction.
    FallbackSlot* victim = &g_pool[0];
    for (auto& s : g_pool) if (s.last_used_ms < victim->last_used_ms) victim = &s;
    ESP_LOGW(TAG, "pool full — evicting ieee=0x%016llx",
              static_cast<unsigned long long>(victim->ieee));
    *victim = {};
    victim->ieee = ieee;
    victim->last_used_ms = now_ms;
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

bool fz_already_in(FallbackSlot& s, const zhc::FzConverter* fz) {
    for (std::size_t i = 0; i < s.n_fz; ++i) if (s.fz[i] == fz) return true;
    return false;
}

void push_fz(FallbackSlot& s, const zhc::FzConverter* fz) {
    if (!fz || fz_already_in(s, fz) || s.n_fz >= kMaxFz) return;
    s.fz[s.n_fz++] = fz;
}

void push_tz(FallbackSlot& s, const zhc::TzConverter* tz) {
    if (!tz || s.n_tz >= kMaxTz) return;
    for (std::size_t i = 0; i < s.n_tz; ++i) if (s.tz[i] == tz) return;
    s.tz[s.n_tz++] = tz;
}

void push_expose(FallbackSlot& s, const zhc::Expose& e) {
    if (s.n_exposes >= kMaxExposes) return;
    s.exposes[s.n_exposes++] = e;
}

void push_binding(FallbackSlot& s, std::uint8_t ep, std::uint16_t cluster) {
    if (s.n_bindings >= kMaxBindings) return;
    s.bindings[s.n_bindings++] = { ep, cluster };
}

void push_report(FallbackSlot& s, std::uint8_t ep, std::uint16_t cluster,
                  std::uint16_t attr, std::uint8_t type,
                  std::uint16_t mn, std::uint16_t mx, std::uint32_t change) {
    if (s.n_reports >= kMaxReports) return;
    s.reports[s.n_reports++] = { ep, cluster, attr, type, mn, mx, change, 0 };
}

const std::uint8_t* push_read_attrs(FallbackSlot& s,
                                     const std::uint16_t* attrs,
                                     std::size_t count) {
    // Encode `count` attr ids LE into scratch; return pointer into scratch.
    const std::size_t need = count * 2;
    if (s.n_scratch + need > kScratchCap) return nullptr;
    std::uint8_t* p = s.scratch + s.n_scratch;
    for (std::size_t i = 0; i < count; ++i) {
        p[2 * i + 0] = static_cast<std::uint8_t>(attrs[i] & 0xFF);
        p[2 * i + 1] = static_cast<std::uint8_t>((attrs[i] >> 8) & 0xFF);
    }
    s.n_scratch += need;
    return p;
}

void push_read_step(FallbackSlot& s, std::uint8_t ep, std::uint16_t cluster,
                     const std::uint16_t* attrs, std::size_t count) {
    if (s.n_steps >= kMaxSteps) return;
    const std::uint8_t* payload = push_read_attrs(s, attrs, count);
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
    s.steps[s.n_steps++] = step;
}

// ─── Cluster emitters ──────────────────────────────────────────────
//
// Each emitter fires only if the device actually has the cluster.
// Reuses the library's generic converters (no new FZ/TZ code needed).

void emit_on_off(FallbackSlot& s, const zhc::PreparedDefinition* base) {
    if (base_handles_cluster(base, 0x0006)) return;
    const std::uint8_t ep = first_endpoint_with_cluster(s, 0x0006);
    if (!ep) return;
    push_expose(s, { "state", zhc::ExposeType::Binary, zhc::Access::StateSet,
                       nullptr, nullptr, nullptr, 0,
                       zhc::ExposeCategory::State });
    push_fz(s, &zhc::generic::kFzOnOff);
    push_tz(s, &zhc::generic::kTzOnOff);
    push_binding(s, ep, 0x0006);
    push_report(s, ep, 0x0006, 0x0000, /*bool*/ 0x10, 0, 3600, 1);
    static constexpr std::uint16_t kReadOnOff[] = { 0x0000 };
    push_read_step(s, ep, 0x0006, kReadOnOff, 1);
}

void emit_level_ctrl(FallbackSlot& s, const zhc::PreparedDefinition* base) {
    if (base_handles_cluster(base, 0x0008)) return;
    const std::uint8_t ep = first_endpoint_with_cluster(s, 0x0008);
    if (!ep) return;
    push_expose(s, { "brightness", zhc::ExposeType::Numeric,
                       zhc::Access::StateSet, nullptr, nullptr, nullptr, 0,
                       zhc::ExposeCategory::State });
    push_fz(s, &zhc::generic::kFzBrightness);
    push_tz(s, &zhc::generic::kTzBrightness);
    push_binding(s, ep, 0x0008);
    push_report(s, ep, 0x0008, 0x0000, /*u8*/ 0x20, 5, 3600, 1);
    static constexpr std::uint16_t kReadLevel[] = { 0x0000 };
    push_read_step(s, ep, 0x0008, kReadLevel, 1);
}

void emit_color_ctrl(FallbackSlot& s, const zhc::PreparedDefinition* base) {
    if (base_handles_cluster(base, 0x0300)) return;
    const std::uint8_t ep = first_endpoint_with_cluster(s, 0x0300);
    if (!ep) return;
    push_expose(s, { "color_temp", zhc::ExposeType::Numeric,
                       zhc::Access::StateSet, "mired", nullptr, nullptr, 0,
                       zhc::ExposeCategory::State });
    push_expose(s, { "color_x",    zhc::ExposeType::Numeric,
                       zhc::Access::State, nullptr, nullptr, nullptr, 0,
                       zhc::ExposeCategory::State });
    push_expose(s, { "color_y",    zhc::ExposeType::Numeric,
                       zhc::Access::State, nullptr, nullptr, nullptr, 0,
                       zhc::ExposeCategory::State });
    push_expose(s, { "color_mode", zhc::ExposeType::Enum,
                       zhc::Access::State, nullptr, nullptr,
                       kColorModeEnum,
                       sizeof(kColorModeEnum) / sizeof(kColorModeEnum[0]),
                       zhc::ExposeCategory::State });
    push_fz(s, &zhc::generic::kFzColorTemperature);
    push_fz(s, &zhc::generic::kFzColor);
    push_tz(s, &zhc::generic::kTzColorTemp);
    push_binding(s, ep, 0x0300);
    push_report(s, ep, 0x0300, 0x0007, /*u16*/ 0x21, 5, 3600, 1);
    static constexpr std::uint16_t kReadColor[] = { 0x0003, 0x0004, 0x0007, 0x0008 };
    push_read_step(s, ep, 0x0300, kReadColor, 4);
}

void emit_power_cfg(FallbackSlot& s, const zhc::PreparedDefinition* base) {
    if (base_handles_cluster(base, 0x0001)) return;
    const std::uint8_t ep = first_endpoint_with_cluster(s, 0x0001);
    if (!ep) return;
    push_expose(s, { "voltage", zhc::ExposeType::Numeric,
                       zhc::Access::State, "mV", nullptr, nullptr, 0,
                       zhc::ExposeCategory::Diagnostic });
    push_expose(s, { "battery", zhc::ExposeType::Numeric,
                       zhc::Access::State, "%", nullptr, nullptr, 0,
                       zhc::ExposeCategory::Diagnostic });
    push_fz(s, &zhc::generic::kFzBattery);
    push_binding(s, ep, 0x0001);
    push_report(s, ep, 0x0001, 0x0020, /*u8*/ 0x20, 3600, 65535, 1);
    push_report(s, ep, 0x0001, 0x0021, /*u8*/ 0x20, 3600, 65535, 1);
    static constexpr std::uint16_t kReadBatt[] = { 0x0020, 0x0021 };
    push_read_step(s, ep, 0x0001, kReadBatt, 2);
}

void emit_temp_meas(FallbackSlot& s, const zhc::PreparedDefinition* base) {
    if (base_handles_cluster(base, 0x0402)) return;
    const std::uint8_t ep = first_endpoint_with_cluster(s, 0x0402);
    if (!ep) return;
    push_expose(s, { "temperature", zhc::ExposeType::Numeric,
                       zhc::Access::State, "°C", nullptr, nullptr, 0,
                       zhc::ExposeCategory::State });
    push_fz(s, &zhc::generic::kFzTemperature);
    push_binding(s, ep, 0x0402);
    push_report(s, ep, 0x0402, 0x0000, /*i16*/ 0x29, 30, 3600, 100);
    static constexpr std::uint16_t kReadT[] = { 0x0000 };
    push_read_step(s, ep, 0x0402, kReadT, 1);
}

void emit_hum_meas(FallbackSlot& s, const zhc::PreparedDefinition* base) {
    if (base_handles_cluster(base, 0x0405)) return;
    const std::uint8_t ep = first_endpoint_with_cluster(s, 0x0405);
    if (!ep) return;
    push_expose(s, { "humidity", zhc::ExposeType::Numeric,
                       zhc::Access::State, "%", nullptr, nullptr, 0,
                       zhc::ExposeCategory::State });
    push_fz(s, &zhc::generic::kFzHumidity);
    push_binding(s, ep, 0x0405);
    push_report(s, ep, 0x0405, 0x0000, /*u16*/ 0x21, 30, 3600, 100);
    static constexpr std::uint16_t kReadH[] = { 0x0000 };
    push_read_step(s, ep, 0x0405, kReadH, 1);
}

void emit_pressure_meas(FallbackSlot& s, const zhc::PreparedDefinition* base) {
    if (base_handles_cluster(base, 0x0403)) return;
    const std::uint8_t ep = first_endpoint_with_cluster(s, 0x0403);
    if (!ep) return;
    push_expose(s, { "pressure", zhc::ExposeType::Numeric,
                       zhc::Access::State, "hPa", nullptr, nullptr, 0,
                       zhc::ExposeCategory::State });
    push_fz(s, &zhc::generic::kFzPressure);
    push_binding(s, ep, 0x0403);
    push_report(s, ep, 0x0403, 0x0000, /*i16*/ 0x29, 30, 3600, 100);
    static constexpr std::uint16_t kReadP[] = { 0x0000 };
    push_read_step(s, ep, 0x0403, kReadP, 1);
}

void emit_illuminance(FallbackSlot& s, const zhc::PreparedDefinition* base) {
    if (base_handles_cluster(base, 0x0400)) return;
    const std::uint8_t ep = first_endpoint_with_cluster(s, 0x0400);
    if (!ep) return;
    push_expose(s, { "illuminance", zhc::ExposeType::Numeric,
                       zhc::Access::State, "lux", nullptr, nullptr, 0,
                       zhc::ExposeCategory::State });
    push_fz(s, &zhc::generic::kFzIlluminance);
    push_binding(s, ep, 0x0400);
    push_report(s, ep, 0x0400, 0x0000, /*u16*/ 0x21, 30, 3600, 100);
    static constexpr std::uint16_t kReadL[] = { 0x0000 };
    push_read_step(s, ep, 0x0400, kReadL, 1);
}

void rebuild(FallbackSlot& s, const char* model, const char* manufacturer,
              const zhc::PreparedDefinition* base) {
    // Reset arrays, keep endpoint data.
    s.n_exposes = 0;
    s.n_fz = 0;
    s.n_tz = 0;
    s.n_bindings = 0;
    s.n_reports = 0;
    s.n_steps = 0;
    s.n_scratch = 0;

    emit_on_off(s, base);
    emit_level_ctrl(s, base);
    emit_color_ctrl(s, base);
    emit_power_cfg(s, base);
    emit_temp_meas(s, base);
    emit_hum_meas(s, base);
    emit_pressure_meas(s, base);
    emit_illuminance(s, base);

    // Stash the model / manufacturer so registry-style log messages
    // don't show "-/-" for generic devices.
    std::snprintf(s.model,        sizeof(s.model),
                   "%s", model        ? model        : "generic");
    std::snprintf(s.manufacturer, sizeof(s.manufacturer),
                   "%s", manufacturer ? manufacturer : "generic");

    // Stitch the PreparedDefinition. Most fields stay null — we don't
    // touch meta, white_labels, on_event, etc. from a fallback.
    s.def = zhc::PreparedDefinition{};
    s.def.zigbee_models        = nullptr;
    s.def.zigbee_models_count  = 0;
    s.def.manufacturer_names   = nullptr;
    s.def.manufacturer_names_count = 0;
    s.def.manufacturer_name_prefix = nullptr;
    s.def.model                = s.model;
    s.def.vendor               = s.manufacturer;
    s.def.exposes              = s.exposes;
    s.def.exposes_count        = static_cast<std::uint8_t>(s.n_exposes);
    s.def.from_zigbee          = s.fz;
    s.def.from_zigbee_count    = static_cast<std::uint8_t>(s.n_fz);
    s.def.to_zigbee            = s.tz;
    s.def.to_zigbee_count      = static_cast<std::uint8_t>(s.n_tz);
    s.def.bindings             = s.bindings;
    s.def.bindings_count       = static_cast<std::uint8_t>(s.n_bindings);
    s.def.reports              = s.reports;
    s.def.reports_count        = static_cast<std::uint8_t>(s.n_reports);
    s.def.config_steps         = s.steps;
    s.def.config_steps_count   = static_cast<std::uint8_t>(s.n_steps);

    s.built = true;
    const bool any = s.n_exposes || s.n_fz || s.n_tz ||
                     s.n_bindings || s.n_reports || s.n_steps;
    if (any) {
        ESP_LOGI(TAG,
                  "[fallback] synth def ieee=0x%016llx exposes=%zu fz=%zu tz=%zu "
                  "bindings=%zu reports=%zu steps=%zu",
                  static_cast<unsigned long long>(s.ieee),
                  s.n_exposes, s.n_fz, s.n_tz,
                  s.n_bindings, s.n_reports, s.n_steps);
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

    FallbackSlot* s = alloc_slot(ieee, /*now_ms*/ 0);
    if (!s) return;

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

    // Force rebuild on next synth_definition() call.
    s->built = false;
}

void clear(std::uint64_t ieee) {
    if (FallbackSlot* s = find_slot(ieee)) *s = {};
}

bool has_data(std::uint64_t ieee) {
    const FallbackSlot* s = find_slot(ieee);
    if (!s) return false;
    for (const auto& ep : s->eps) if (ep.endpoint != 0) return true;
    return false;
}

const zhc::PreparedDefinition* synth_definition(std::uint64_t ieee,
                                                 const char* model,
                                                 const char* manufacturer,
                                                 const zhc::PreparedDefinition* base) {
    FallbackSlot* s = find_slot(ieee);
    if (!s) return nullptr;
    // `base` affects what gets emitted, so rebuild unconditionally when
    // a base is supplied — the previous rebuild may have been for a
    // different base (or no base at all). Cheap: the slot data is tiny.
    rebuild(*s, model, manufacturer, base);
    if (s->n_exposes == 0) {
        // No mapped clusters left after subtracting `base`'s coverage
        // — either a pure router, unsupported device type, or the
        // registry def already handles everything.
        return nullptr;
    }
    return &s->def;
}

}  // namespace zhc_fallback
