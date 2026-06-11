// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "zhc_adapter.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "zap_common.h"   // ZAP_MAX_DEVICES — single source of device-count truth

#include "metrics/metrics_macros.h"

#include "zhc/runtime/expose_range.hpp"

#include "definitions/lumi/_shared.hpp"
#include "zhc/devices/lumi_registry.hpp"
#include "zhc/devices/efekta_registry.hpp"
#include "zhc/devices/adeo_registry.hpp"
#include "zhc/devices/develco_registry.hpp"
#include "zhc/devices/feibit_registry.hpp"
#include "zhc/devices/gledopto_registry.hpp"
#include "zhc/devices/hive_registry.hpp"
#include "zhc/devices/iluminize_registry.hpp"
#include "zhc/devices/legrand_registry.hpp"
#include "zhc/devices/muller_licht_registry.hpp"
#include "zhc/devices/nue_3a_registry.hpp"
#include "zhc/devices/orvibo_registry.hpp"
#include "zhc/devices/paulmann_registry.hpp"
#include "zhc/devices/robb_registry.hpp"
#include "zhc/devices/shinasystem_registry.hpp"
#include "zhc/devices/slacky_diy_registry.hpp"
#include "zhc/devices/third_reality_registry.hpp"
// Tier D registries.
#include "zhc/devices/yale_registry.hpp"
#include "zhc/devices/sengled_registry.hpp"
#include "zhc/devices/smartthings_registry.hpp"
#include "zhc/devices/yokis_registry.hpp"
#include "zhc/devices/zemismart_registry.hpp"
#include "zhc/devices/ewelink_registry.hpp"
#include "zhc/devices/candeo_registry.hpp"
#include "zhc/devices/sylvania_registry.hpp"
#include "zhc/devices/sinope_registry.hpp"
#include "zhc/devices/qa_registry.hpp"
#include "zhc/devices/lincukoo_registry.hpp"
#include "zhc/devices/bitron_registry.hpp"
#include "zhc/devices/aurora_lighting_registry.hpp"
#include "zhc/devices/immax_registry.hpp"
#include "zhc/devices/bosch_registry.hpp"
#include "zhc/devices/shelly_registry.hpp"
#include "zhc/devices/owon_registry.hpp"
#include "zhc/devices/adurosmart_registry.hpp"
#include "zhc/devices/tier_e_registries.hpp"
#include "zhc/devices/heiman_registry.hpp"
#include "zhc/devices/ikea_registry.hpp"
#include "zhc/devices/innr_registry.hpp"
#include "zhc/devices/ledvance_registry.hpp"
#include "zhc/devices/namron_registry.hpp"
#include "zhc/devices/osram_registry.hpp"
#include "zhc/devices/sunricher_registry.hpp"
#include "zhc/devices/moes_registry.hpp"
#include "zhc/devices/philips_registry.hpp"
#include "zhc/devices/saswell_registry.hpp"
#include "zhc/devices/schneider_registry.hpp"
#include "zhc/devices/sonoff_registry.hpp"
#include "zhc/devices/tuya_registry.hpp"
#include "zhc/runtime/definition.hpp"
#include "zhc/runtime/definition_runtime.hpp"
#include "zhc/runtime/dispatch.hpp"
#include "zhc/runtime/store.hpp"
#include "zhc/runtime/timer.hpp"
#include "zhc/types.hpp"
#include "zhc/cluster_names.hpp"
#include "zhc/zcl/decoder.hpp"
#include "zhc/zcl/foundation.hpp"
#include "zhc/zcl/header.hpp"

#include "zhc_adapter_fallback.hpp"
#include "zhc_adapter_internal.hpp"

static const char* TAG = "zhc_adapter";

namespace {

// Wall-clock hook exposed to the library. Returns ms since boot.
std::uint32_t now_ms() {
    return static_cast<std::uint32_t>(esp_timer_get_time() / 1000ULL);
}

// Shared runtime state store — one DeviceRuntimeState per joined device,
// indexed by the per-IEEE slot index (see IeeeSlot below). Was pinned to
// 32 (P2-T13 finding): the network can hold up to ZAP_MAX_DEVICES (200),
// so device 33+ aliased onto slot index 0 and corrupted converter state
// (press/hold timers, Tuya action de-dup) shared across unrelated devices.
// Sized to the canonical ZAP_MAX_DEVICES so the slot index is always
// in-range and every device gets its own runtime state + def cache.
constexpr std::size_t kMaxDevices = ZAP_MAX_DEVICES;
// PSRAM-resident (EXT_RAM_BSS_ATTR): RuntimeStore<200> is ~11 KB and the
// store is only touched from warm task context (decode / configure /
// command), never from an ISR or DMA — so external RAM is safe and keeps
// the 200-entry bump off the tight internal DRAM budget (S3 dram0). The
// `= {}` initialiser is dropped (startup zeroes .ext_ram.bss), matching
// the g_merged convention.
EXT_RAM_BSS_ATTR zhc::RuntimeStore<kMaxDevices> g_store;

// ── ESP-IDF timer scheduler ──────────────────────────────────────────
// Fixed-capacity pool of one-shot esp_timer handles. Mirrors the host
// fake's interface so converters that depend on TimerScheduler run
// identically on host tests and on hardware.
constexpr std::size_t kMaxIdfTimers = 16;

struct IdfTimerSlot {
    esp_timer_handle_t handle;
    std::uint16_t      device_index;
    std::uint32_t      user_tag;
    zhc::TimerFiredFn  fn;
    void*              user_data;
    // Generation tag (P2-T13 finding, def 7): bumped on every (de)alloc
    // of the slot. A TimerId carries the generation it was minted at so a
    // late cancel of a slot that has since fired + been reused for a
    // different timer is rejected (ABA guard). The id is the low 16 bits
    // (slot index +1) packed with the generation in the high 16 bits.
    std::uint16_t      generation;
    bool               in_use;
};

IdfTimerSlot g_timer_slots[kMaxIdfTimers] = {};

// Timer slots are mutated from two contexts: the caller (decode /
// configure task) via idf_timer_schedule / idf_timer_cancel, and the
// esp_timer service task via idf_timer_fire_thunk. The `in_use` /
// `handle` / `generation` fields race between them — a portMUX critical
// section (not a mutex: fire_thunk runs in a high-prio service task and
// must not block) serialises the transitions. Matches the spinlock
// pattern used in znp_driver (znp_worker.cpp s_slot_mux).
portMUX_TYPE g_timer_mux = portMUX_INITIALIZER_UNLOCKED;

// Pack/unpack a TimerId from (slot index, generation). ids are 1-based on
// the slot so 0 stays reserved for kInvalidTimerId.
inline zhc::TimerId timer_id_pack(std::size_t i, std::uint16_t gen) {
    return static_cast<zhc::TimerId>(
        ((static_cast<std::uint32_t>(gen) << 16) |
         (static_cast<std::uint32_t>(i) + 1)));
}
inline std::size_t   timer_id_slot(zhc::TimerId id) {
    return static_cast<std::size_t>((static_cast<std::uint32_t>(id) & 0xFFFFu) - 1);
}
inline std::uint16_t timer_id_gen(zhc::TimerId id) {
    return static_cast<std::uint16_t>(static_cast<std::uint32_t>(id) >> 16);
}

void idf_timer_fire_thunk(void* arg) {
    auto* slot = static_cast<IdfTimerSlot*>(arg);
    if (!slot) return;
    // Snapshot the user callback args + release the slot under the mux so
    // a concurrent cancel can't double-delete the handle. The fn itself
    // runs OUTSIDE the critical section (it may re-enter the timer API).
    zhc::TimerFiredFn  fn        = nullptr;
    std::uint16_t      dev_index = 0;
    std::uint32_t      tag       = 0;
    void*              user      = nullptr;
    esp_timer_handle_t to_delete = nullptr;
    portENTER_CRITICAL(&g_timer_mux);
    if (slot->in_use) {
        fn        = slot->fn;
        dev_index = slot->device_index;
        tag       = slot->user_tag;
        user      = slot->user_data;
        to_delete = slot->handle;
        slot->handle = nullptr;
        slot->in_use = false;
        ++slot->generation;   // invalidate any outstanding TimerId for this slot
    }
    portEXIT_CRITICAL(&g_timer_mux);
    if (to_delete) esp_timer_delete(to_delete);
    if (fn) fn(dev_index, tag, user);
}

zhc::TimerId idf_timer_schedule(void*,
                                 std::uint32_t delay_ms,
                                 std::uint16_t device_index,
                                 std::uint32_t user_tag,
                                 zhc::TimerFiredFn fn,
                                 void* user_data) {
    for (std::size_t i = 0; i < kMaxIdfTimers; ++i) {
        std::uint16_t gen;
        portENTER_CRITICAL(&g_timer_mux);
        if (g_timer_slots[i].in_use) { portEXIT_CRITICAL(&g_timer_mux); continue; }
        auto& s = g_timer_slots[i];
        s.device_index = device_index;
        s.user_tag     = user_tag;
        s.fn           = fn;
        s.user_data    = user_data;
        s.handle       = nullptr;
        s.in_use       = true;
        gen            = s.generation;
        portEXIT_CRITICAL(&g_timer_mux);

        esp_timer_create_args_t args{};
        args.callback        = &idf_timer_fire_thunk;
        args.arg             = &g_timer_slots[i];
        args.dispatch_method = ESP_TIMER_TASK;
        args.name            = "zhc";
        esp_timer_handle_t h = nullptr;
        if (esp_timer_create(&args, &h) != ESP_OK) {
            portENTER_CRITICAL(&g_timer_mux);
            g_timer_slots[i].in_use = false;
            ++g_timer_slots[i].generation;
            portEXIT_CRITICAL(&g_timer_mux);
            return zhc::kInvalidTimerId;
        }
        // Publish the handle before arming so a fire/cancel sees it.
        portENTER_CRITICAL(&g_timer_mux);
        g_timer_slots[i].handle = h;
        portEXIT_CRITICAL(&g_timer_mux);
        if (esp_timer_start_once(h,
                                   static_cast<uint64_t>(delay_ms) * 1000ULL) != ESP_OK) {
            portENTER_CRITICAL(&g_timer_mux);
            g_timer_slots[i].handle = nullptr;
            g_timer_slots[i].in_use = false;
            ++g_timer_slots[i].generation;
            portEXIT_CRITICAL(&g_timer_mux);
            esp_timer_delete(h);
            return zhc::kInvalidTimerId;
        }
        return timer_id_pack(i, gen);
    }
    ESP_LOGW("zhc_adapter", "timer pool exhausted (cap=%zu)", kMaxIdfTimers);
    return zhc::kInvalidTimerId;
}

void idf_timer_cancel(void*, zhc::TimerId id) {
    if (id == zhc::kInvalidTimerId) return;
    const std::size_t i = timer_id_slot(id);
    if (i >= kMaxIdfTimers) return;
    const std::uint16_t want_gen = timer_id_gen(id);
    esp_timer_handle_t to_delete = nullptr;
    portENTER_CRITICAL(&g_timer_mux);
    auto& s = g_timer_slots[i];
    // ABA guard: reject the cancel if the slot has been reused since this
    // id was minted (fired + rescheduled for a different timer).
    if (s.in_use && s.generation == want_gen) {
        to_delete = s.handle;
        s.handle  = nullptr;
        s.in_use  = false;
        ++s.generation;
    }
    portEXIT_CRITICAL(&g_timer_mux);
    if (to_delete) {
        esp_timer_stop(to_delete);
        esp_timer_delete(to_delete);
    }
}

zhc::TimerScheduler g_timer_scheduler{
    .schedule = &idf_timer_schedule,
    .cancel   = &idf_timer_cancel,
    .impl     = nullptr,
};

// Device-index allocator keyed on IEEE. Small linear table; swap for
// a hashmap if the device count grows past a few dozen.
//
// `cached_def` piggy-backs on the same table — populated on the first
// `zhac_adapter_try_decode` for a given ieee and reused for every
// subsequent frame. Without it each frame walks the 5000+ merged
// registry through 3 strcmp passes. nwk_addr is not a safe key —
// rejoins rotate it — so we cache by ieee here in the existing slot.
//
// Concurrency (P2-T13 findings, defs 2/3/4):
//  * The table is APPEND-ONLY. A device's slot index, once assigned, is
//    stable for the life of the process — slots are never moved, recycled
//    or compacted (`g_slot_count` only grows). This is what lets the
//    configure_* bridges resolve their per-call address by `device_index`
//    alone (ieee = the slot's key field; nwk = cfg_nwk below).
//  * `s_slots_mtx` guards `g_slot_count` (append) and the linear scan in
//    find_slot_locked/resolve_device_index. Hold times are short — NO registry
//    walk, synth rebuild or radio I/O ever runs under it.
//  * Lock order vs the fallback pool mutex (g_pool_mtx): the two are
//    NEVER nested. Slot resolution (s_slots_mtx) completes and releases
//    BEFORE any synth/owns() call takes g_pool_mtx, and the eviction path
//    that calls invalidate_cached_defs_in holds g_pool_mtx but touches
//    g_slots WITHOUT taking s_slots_mtx (it relies on word-atomic pointer
//    clears + the append-only invariant — the same relaxed-reader pattern
//    T4 documented). So no inversion is possible.
struct IeeeSlot {
    std::uint64_t                  ieee;
    std::uint16_t                  idx;
    // Per-call addressing (def 3+4). Latched by zhac_adapter_set_runtime_addr
    // (radio task, before try_decode) and by zhac_adapter_configure, keyed
    // on this slot's device_index. The configure_* bridges read THESE —
    // indexed by the library-supplied device_index — instead of a pair of
    // cross-call module globals, so a configure on one device and a decode
    // on another can run in parallel without clobbering each other's
    // destination address. nwk rotates on rejoin; ieee is the stable key.
    std::uint16_t                  cfg_nwk;
    const zhc::PreparedDefinition* cached_def;
    // Supplementary fallback def — populated when a registry match
    // exists but the device advertises additional clusters the
    // registry def doesn't bind (DIY fork of a known device). Null
    // when the primary def covers everything the fallback would have
    // emitted, or when the primary IS the fallback (pure synth path).
    const zhc::PreparedDefinition* cached_supplement;
};
// PSRAM-resident (EXT_RAM_BSS_ATTR): IeeeSlot is ~32 B; ZAP_MAX_DEVICES
// (200) entries ≈ 6.4 KB. Warm-path only (task context), never ISR — so
// external RAM is safe, matching g_merged / g_store. `= {}` dropped
// (.ext_ram.bss is zeroed at startup).
EXT_RAM_BSS_ATTR IeeeSlot g_slots[kMaxDevices];
std::size_t g_slot_count = 0;

// Returned by resolve_device_index when the (now ZAP_MAX_DEVICES-sized)
// table is genuinely full. NEVER alias to 0 — that is a real device's
// slot and aliasing corrupts its converter state (the bug this fixes).
constexpr std::uint16_t kInvalidDeviceIndex = 0xFFFF;

// Slot-table mutex (def 2). Created at static-init time (global ctor),
// matching the fallback pool's PoolMtxInit style — NOT lazily, so the
// very first radio frame already has a valid lock. See lock-order note
// on IeeeSlot above.
SemaphoreHandle_t s_slots_mtx = nullptr;
struct SlotsMtxInit { SlotsMtxInit() { s_slots_mtx = xSemaphoreCreateMutex(); } };
SlotsMtxInit s_slots_mtx_init;

struct SlotsLock {
    SlotsLock()  { if (s_slots_mtx) xSemaphoreTake(s_slots_mtx, portMAX_DELAY); }
    ~SlotsLock() { if (s_slots_mtx) xSemaphoreGive(s_slots_mtx); }
    SlotsLock(const SlotsLock&)            = delete;
    SlotsLock& operator=(const SlotsLock&) = delete;
};

// Caller must hold s_slots_mtx. Append-only scan.
IeeeSlot* find_slot_locked(std::uint64_t ieee) {
    for (std::size_t i = 0; i < g_slot_count; ++i) {
        if (g_slots[i].ieee == ieee) return &g_slots[i];
    }
    return nullptr;
}

// Returns the stable slot index for `ieee`, allocating on first sight.
// Returns kInvalidDeviceIndex iff the table is full (caller must treat
// that as "drop this device" — never index a real slot).
std::uint16_t resolve_device_index(std::uint64_t ieee) {
    SlotsLock lock;
    if (auto* s = find_slot_locked(ieee)) return s->idx;
    if (g_slot_count < kMaxDevices) {
        const auto idx = static_cast<std::uint16_t>(g_slot_count);
        g_slots[g_slot_count].ieee              = ieee;
        g_slots[g_slot_count].idx               = idx;
        g_slots[g_slot_count].cfg_nwk           = 0;
        g_slots[g_slot_count].cached_def        = nullptr;
        g_slots[g_slot_count].cached_supplement = nullptr;
        ++g_slot_count;
        return idx;
    }
    static bool warned = false;
    if (!warned) {
        warned = true;
        ESP_LOGE(TAG, "slot table full (cap=%zu) — device 0x%016llx dropped; "
                       "bump kMaxDevices/ZAP_MAX_DEVICES if the network grew",
                  kMaxDevices, static_cast<unsigned long long>(ieee));
    }
    return kInvalidDeviceIndex;
}

// Negative-match sentinel for cached_def (def 5). A distinct, non-null,
// non-dereferenceable pointer meaning "we resolved this ieee and it
// matched NO definition" — so subsequent frames skip the 5500-def
// registry walk instead of re-walking + re-logging every frame. Kept
// separate from the cached_supplement==primary sentinel (def 8, which
// means "no SUPPLEMENT needed"). Invalidated on register_endpoint /
// fallback_clear / invalidate_def_cache exactly like a real cached_def,
// so a device that later DOES match (new cluster data → synth) is not
// masked. The address of a file-scope object is unique and never a valid
// PreparedDefinition*, so it can never collide with a registry/pool def.
char g_miss_sentinel_storage = 0;
const zhc::PreparedDefinition* const kMissSentinel =
    reinterpret_cast<const zhc::PreparedDefinition*>(&g_miss_sentinel_storage);

// Drop the cached def + supplement for one ieee (takes s_slots_mtx).
// Used by every invalidation hook (register_endpoint, fallback_clear,
// invalidate_def_cache). Clears the negative-miss sentinel too, so a
// device that newly matches is re-resolved on the next frame.
void clear_cached_defs_for(std::uint64_t ieee) {
    SlotsLock lock;
    if (auto* slot = find_slot_locked(ieee)) {
        slot->cached_def        = nullptr;
        slot->cached_supplement = nullptr;
    }
}

// Snapshot of a slot's cached state, taken under s_slots_mtx and copied
// out so callers never dereference g_slots without the lock. `present`
// is false when no slot exists for the ieee yet.
struct SlotSnapshot {
    bool                           present;
    std::uint16_t                  idx;
    std::uint16_t                  cfg_nwk;
    const zhc::PreparedDefinition* cached_def;
    const zhc::PreparedDefinition* cached_supplement;
};

SlotSnapshot snapshot_slot(std::uint64_t ieee) {
    SlotsLock lock;
    if (auto* s = find_slot_locked(ieee)) {
        return { true, s->idx, s->cfg_nwk, s->cached_def, s->cached_supplement };
    }
    return { false, kInvalidDeviceIndex, 0, nullptr, nullptr };
}

// Store the resolved def/supplement back into the slot (takes the lock).
void store_cached_defs(std::uint64_t ieee,
                       const zhc::PreparedDefinition* def,
                       const zhc::PreparedDefinition* supp) {
    SlotsLock lock;
    if (auto* s = find_slot_locked(ieee)) {
        s->cached_def        = def;
        s->cached_supplement = supp;
    }
}

// Merged registry view built from every vendor's kXxxRegistry[].
// Copied once at init; walking a single span is simpler than threading
// two spans through every call site.
// PSRAM-resident via EXT_RAM_BSS_ATTR (32 KB of pointers — the old
// comment claimed PSRAM but the array actually sat in internal .bss).
// Zero-initialised by startup like any .bss; no initialiser allowed.
// Walked on device resolve/diagnostics only — never ISR/per-byte hot.
// Sized generously — bump whenever the total nears the cap.
// Total ported defs ≈ 5500+ and growing. Cap must stay above the sum
// or the last-added vendors (tier_e → miboxer, aurora_lighting, …)
// are silently truncated and lookups mis-route to earlier stubs.
constexpr std::size_t kMaxRegistry = 8192;
EXT_RAM_BSS_ATTR const zhc::PreparedDefinition* g_merged[kMaxRegistry];
std::size_t g_merged_count = 0;

void merge_registries() {
    g_merged_count = 0;
    const auto add = [](const zhc::PreparedDefinition* const* src,
                        std::size_t n) {
        for (std::size_t i = 0; i < n && g_merged_count < kMaxRegistry; ++i) {
            g_merged[g_merged_count++] = src[i];
        }
    };
    add(zhc::devices::lumi::kLumiRegistry,
        zhc::devices::lumi::kLumiRegistryCount);
    add(zhc::devices::tuya::kTuyaRegistry,
        zhc::devices::tuya::kTuyaRegistryCount);
    add(zhc::devices::moes::kMoesRegistry,
        zhc::devices::moes::kMoesRegistryCount);
    add(zhc::devices::philips::kPhilipsRegistry,
        zhc::devices::philips::kPhilipsRegistryCount);
    add(zhc::devices::sonoff::kSonoffRegistry,
        zhc::devices::sonoff::kSonoffRegistryCount);
    add(zhc::devices::heiman::kHeimanRegistry,
        zhc::devices::heiman::kHeimanRegistryCount);
    add(zhc::devices::saswell::kSaswellRegistry,
        zhc::devices::saswell::kSaswellRegistryCount);
    add(zhc::devices::efekta::kEfektaRegistry,
        zhc::devices::efekta::kEfektaRegistryCount);
    add(zhc::devices::schneider::kSchneiderRegistry,
        zhc::devices::schneider::kSchneiderRegistryCount);
    add(zhc::devices::ikea::kIkeaRegistry,
        zhc::devices::ikea::kIkeaRegistryCount);
    add(zhc::devices::innr::kInnrRegistry,
        zhc::devices::innr::kInnrRegistryCount);
    add(zhc::devices::gledopto::kGledoptoRegistry,
        zhc::devices::gledopto::kGledoptoRegistryCount);
    add(zhc::devices::sunricher::kSunricherRegistry,
        zhc::devices::sunricher::kSunricherRegistryCount);
    add(zhc::devices::namron::kNamronRegistry,
        zhc::devices::namron::kNamronRegistryCount);
    add(zhc::devices::ledvance::kLedvanceRegistry,
        zhc::devices::ledvance::kLedvanceRegistryCount);
    add(zhc::devices::osram::kOsramRegistry,
        zhc::devices::osram::kOsramRegistryCount);
    add(zhc::devices::adeo::kAdeoRegistry,
        zhc::devices::adeo::kAdeoRegistryCount);
    add(zhc::devices::legrand::kLegrandRegistry,
        zhc::devices::legrand::kLegrandRegistryCount);
    add(zhc::devices::shinasystem::kShinasystemRegistry,
        zhc::devices::shinasystem::kShinasystemRegistryCount);
    add(zhc::devices::third_reality::kThirdRealityRegistry,
        zhc::devices::third_reality::kThirdRealityRegistryCount);
    add(zhc::devices::robb::kRobbRegistry,
        zhc::devices::robb::kRobbRegistryCount);
    add(zhc::devices::paulmann::kPaulmannRegistry,
        zhc::devices::paulmann::kPaulmannRegistryCount);
    add(zhc::devices::orvibo::kOrviboRegistry,
        zhc::devices::orvibo::kOrviboRegistryCount);
    add(zhc::devices::slacky_diy::kSlackyDiyRegistry,
        zhc::devices::slacky_diy::kSlackyDiyRegistryCount);
    add(zhc::devices::nue_3a::kNue3aRegistry,
        zhc::devices::nue_3a::kNue3aRegistryCount);
    add(zhc::devices::muller_licht::kMullerLichtRegistry,
        zhc::devices::muller_licht::kMullerLichtRegistryCount);
    add(zhc::devices::develco::kDevelcoRegistry,
        zhc::devices::develco::kDevelcoRegistryCount);
    add(zhc::devices::iluminize::kIluminizeRegistry,
        zhc::devices::iluminize::kIluminizeRegistryCount);
    add(zhc::devices::hive::kHiveRegistry,
        zhc::devices::hive::kHiveRegistryCount);
    add(zhc::devices::feibit::kFeibitRegistry,
        zhc::devices::feibit::kFeibitRegistryCount);
    add(zhc::devices::yale::kYaleRegistry,
        zhc::devices::yale::kYaleRegistryCount);
    add(zhc::devices::sengled::kSengledRegistry,
        zhc::devices::sengled::kSengledRegistryCount);
    add(zhc::devices::smartthings::kSmartthingsRegistry,
        zhc::devices::smartthings::kSmartthingsRegistryCount);
    add(zhc::devices::yokis::kYokisRegistry,
        zhc::devices::yokis::kYokisRegistryCount);
    add(zhc::devices::zemismart::kZemismartRegistry,
        zhc::devices::zemismart::kZemismartRegistryCount);
    add(zhc::devices::ewelink::kEwelinkRegistry,
        zhc::devices::ewelink::kEwelinkRegistryCount);
    add(zhc::devices::candeo::kCandeoRegistry,
        zhc::devices::candeo::kCandeoRegistryCount);
    add(zhc::devices::sylvania::kSylvaniaRegistry,
        zhc::devices::sylvania::kSylvaniaRegistryCount);
    add(zhc::devices::sinope::kSinopeRegistry,
        zhc::devices::sinope::kSinopeRegistryCount);
    add(zhc::devices::qa::kQaRegistry,
        zhc::devices::qa::kQaRegistryCount);
    add(zhc::devices::lincukoo::kLincukooRegistry,
        zhc::devices::lincukoo::kLincukooRegistryCount);
    add(zhc::devices::bitron::kBitronRegistry,
        zhc::devices::bitron::kBitronRegistryCount);
    add(zhc::devices::aurora_lighting::kAuroraLightingRegistry,
        zhc::devices::aurora_lighting::kAuroraLightingRegistryCount);
    add(zhc::devices::immax::kImmaxRegistry,
        zhc::devices::immax::kImmaxRegistryCount);
    add(zhc::devices::bosch::kBoschRegistry,
        zhc::devices::bosch::kBoschRegistryCount);
    add(zhc::devices::shelly::kShellyRegistry,
        zhc::devices::shelly::kShellyRegistryCount);
    add(zhc::devices::owon::kOwonRegistry,
        zhc::devices::owon::kOwonRegistryCount);
    add(zhc::devices::adurosmart::kAdurosmartRegistry,
        zhc::devices::adurosmart::kAdurosmartRegistryCount);
    // Tier E — 323 long-tail vendors iterated via aggregator.
    for (std::size_t i = 0;
         i < zhc::devices::tier_e::kTierERegistriesCount; ++i) {
        const auto& e = zhc::devices::tier_e::kTierERegistries[i];
        add(e.reg, e.count);
    }
}

const zhc::PreparedDefinition* find_definition(const char* model_id,
                                                 const char* manu_name) {
    std::span<const zhc::PreparedDefinition* const> view(
        g_merged, g_merged_count);
    return zhc::find_definition(model_id, manu_name, view);
}

// Registry first, cluster-based fallback second. `ieee == 0` → registry
// only (used by callers that don't have a device record).
const zhc::PreparedDefinition* resolve_definition(std::uint64_t ieee,
                                                    const char* model_id,
                                                    const char* manu_name) {
    if (const auto* def = find_definition(model_id, manu_name)) return def;
    if (ieee == 0) return nullptr;
    return zhc_fallback::synth_definition(ieee, model_id, manu_name, nullptr);
}

// Build a supplementary fallback def for clusters `primary` doesn't
// bind. Returns nullptr if the primary already covers everything the
// fallback would emit, or if no cluster data was registered for this
// ieee. The returned pointer is owned by the fallback pool.
//
// Only emits when `primary` came from the registry. A primary that is
// itself a fallback synth is already the full view of the device's
// clusters — there's nothing left to add.
//
// Caching: uses slot->cached_supplement as a two-state cache.
//   nullptr          → not yet computed (initial state, or invalidated)
//   == primary       → computed, synth returned nullptr (negative sentinel;
//                      primary covers everything, no supplement needed)
//   anything else    → computed, valid supplement pointer
// The sentinel (primary) is safe because the supplement pointer would
// never legitimately equal the primary (they come from different pools).
const zhc::PreparedDefinition* resolve_supplement(std::uint64_t ieee,
                                                    const char* model_id,
                                                    const char* manu_name,
                                                    const zhc::PreparedDefinition* primary) {
    if (ieee == 0 || !primary) return nullptr;
    if (find_definition(model_id, manu_name) == nullptr) return nullptr;

    // Check per-slot cache before running synth. The snapshot reads the
    // cached_supplement state under s_slots_mtx and copies it out — synth
    // (which takes g_pool_mtx) NEVER runs while we hold the slot lock, so
    // the two mutexes are sequential, never nested.
    const SlotSnapshot snap = snapshot_slot(ieee);
    if (snap.present) {
        if (snap.cached_supplement == primary) {
            // Negative sentinel: previously computed, no supplement needed.
            return nullptr;
        }
        if (snap.cached_supplement != nullptr) {
            // Positive cache hit.
            return snap.cached_supplement;
        }
        // Cache miss — compute (slot lock released), then store result.
        const zhc::PreparedDefinition* result =
            zhc_fallback::synth_definition(ieee, model_id, manu_name, primary);
        // Store sentinel (primary) for negative result, real pointer for positive.
        const zhc::PreparedDefinition* to_store = result ? result : primary;
        // Race (b) closure, same shape as the decode-miss path: synth ran
        // without this caller holding the pool mutex, so the pool entry
        // backing `result` may have been evicted / repurposed between
        // resolve and the store above — and the invalidation walk may
        // have run BEFORE the store landed, missing it. Store first,
        // then re-validate ownership under the pool lock; on failure
        // drop the cached copy so no stale pool pointer persists. The
        // negative sentinel (registry `primary`) is not pool storage and
        // always passes owns(). The local `result` still serves this one
        // call — same one-generation A/B-buffer argument as decode-miss.
        if (!zhc_fallback::owns(ieee, to_store)) {
            to_store = nullptr;
        }
        // Re-find the slot under the lock to store (it can't have moved —
        // append-only — but another invalidation may have cleared it; the
        // store is idempotent either way).
        {
            SlotsLock lock;
            if (auto* slot = find_slot_locked(ieee)) {
                slot->cached_supplement = to_store;
            }
        }
        return result;
    }

    // No slot yet — compute without caching (slot created on next try_decode).
    return zhc_fallback::synth_definition(ieee, model_id, manu_name, primary);
}

// Cluster id → canonical z2m name. Table lives in
// `zhc/cluster_names.hpp` — shared with the host fixture runner so both
// sides cannot silently drift (see Mi-Cube rotate regression).
using zhc::cluster_id_to_name;

// One-line summary of a Value for observational logging.
void format_value(char* out, std::size_t cap, const zhc::Value& v) {
    switch (v.type) {
        case zhc::ValueType::Bool:
            std::snprintf(out, cap, "%s", v.b ? "true" : "false");
            break;
        case zhc::ValueType::Uint:
            std::snprintf(out, cap, "%llu",
                          static_cast<unsigned long long>(v.u));
            break;
        case zhc::ValueType::Int:
            std::snprintf(out, cap, "%lld",
                          static_cast<long long>(v.i));
            break;
        case zhc::ValueType::Float:
            std::snprintf(out, cap, "%.3f", static_cast<double>(v.f));
            break;
        case zhc::ValueType::StringRef:
            std::snprintf(out, cap, "\"%s\"", v.str ? v.str : "");
            break;
        case zhc::ValueType::BytesRef:
            std::snprintf(out, cap, "<%zu bytes>", v.bytes.size());
            break;
        case zhc::ValueType::ObjectRef:
            std::snprintf(out, cap, "<object>");
            break;
        default:
            std::snprintf(out, cap, "?");
            break;
    }
}

void log_payload(std::uint64_t ieee,
                  const zhc::PreparedDefinition& def,
                  const zhc::DispatchResult& result,
                  std::uint16_t cluster_id,
                  const zhc::DecodedMessage* msg,
                  const std::uint8_t* zcl,
                  std::size_t zcl_len) {
    char prefix[80];
    std::snprintf(prefix, sizeof(prefix),
                   "[zhc-lib] 0x%016llx %s/%s",
                   static_cast<unsigned long long>(ieee),
                   def.vendor ? def.vendor : "?",
                   def.model  ? def.model  : "?");
    if (!result.any_matched) {
        // Show why the frame didn't match: cluster, family, command/attr,
        // first 16 bytes of raw ZCL. Lets ports be debugged without a
        // sniffer trace.
        char hex[3 * 16 + 1] = {0};
        const std::size_t n = zcl_len < 16 ? zcl_len : 16;
        for (std::size_t i = 0; i < n; ++i) {
            std::snprintf(hex + i * 3, 4, "%02x ", zcl[i]);
        }
        const char* family_name = "?";
        if (msg) {
            switch (msg->family) {
                case zhc::FrameFamily::Zcl:    family_name = "Zcl"; break;
                case zhc::FrameFamily::TuyaDp: family_name = "TuyaDp"; break;
            }
        }
        // Benign protocol housekeeping carries no device state — demote to
        // debug so it doesn't read like a device-coverage gap:
        //   * any ZCL Default Response (global cmd 0x0B, any cluster)
        //   * Tuya 0xEF00 MCU management — 0x10/0x11 version, 0x24 sync-time,
        //     0x25 gateway-connection-status
        bool protocol_noise = false;
        if (msg) {
            const auto cmd = msg->command_id;
            const bool cluster_specific =
                zcl_len >= 1 && (zcl[0] & 0x03) == 0x01;
            if (!cluster_specific && cmd == 0x0B) {
                protocol_noise = true;
            } else if (cluster_id == 0xEF00 && cluster_specific &&
                       (cmd == 0x10 || cmd == 0x11 ||
                        cmd == 0x24 || cmd == 0x25)) {
                protocol_noise = true;
            }
        }
        if (protocol_noise) {
            ESP_LOGD(TAG, "%s  (protocol frame, no state) cluster=0x%04x cmd=0x%02x",
                     prefix, cluster_id, msg ? msg->command_id : 0);
        } else {
            ESP_LOGI(TAG, "%s  (no match) cluster=0x%04x(%s) family=%s type=%u cmd=0x%02x zcl[%zu]=%s%s",
                     prefix, cluster_id,
                     msg && msg->cluster ? msg->cluster : "?",
                     family_name,
                     msg ? static_cast<unsigned>(msg->type) : 0,
                     msg ? msg->command_id : 0,
                     zcl_len, hex, zcl_len > 16 ? "…" : "");
        }
        return;
    }
    ESP_LOGI(TAG, "%s  matched, %u keys:",
             prefix, static_cast<unsigned>(result.merged.count));
    for (std::uint8_t i = 0; i < result.merged.count; ++i) {
        const auto& kv = result.merged.items[i];
        char buf[48];
        format_value(buf, sizeof(buf), kv.value);
        ESP_LOGI(TAG, "  %s = %s", kv.key ? kv.key : "?", buf);
    }
}

zhac_shadow_update_fn_t g_shadow_update = nullptr;

void fire_shadow_updates(std::uint64_t ieee,
                          const zhc::DispatchResult& result) {
    if (!g_shadow_update) return;
    for (std::uint8_t i = 0; i < result.merged.count; ++i) {
        const auto& kv = result.merged.items[i];
        if (!kv.key) continue;
        const auto& v = kv.value;
        const std::uint8_t kind = static_cast<std::uint8_t>(v.type);
        g_shadow_update(
            ieee, kv.key, kind,
            v.type == zhc::ValueType::Int   ? v.i : 0,
            v.type == zhc::ValueType::Uint  ? v.u : 0,
            v.type == zhc::ValueType::Float ? v.f : 0.0f,
            v.type == zhc::ValueType::Bool  ? v.b : false,
            v.type == zhc::ValueType::StringRef ? v.str : nullptr);
    }
}

}  // namespace

extern "C" void zhac_adapter_register_shadow(zhac_shadow_update_fn_t fn) {
    g_shadow_update = fn;
}

namespace {
zhac_configure_bind_fn_t   g_cfg_bind   = nullptr;
zhac_configure_report_fn_t g_cfg_report = nullptr;
zhac_configure_cmd_fn_t    g_cfg_cmd    = nullptr;
zhac_configure_read_fn_t   g_cfg_read   = nullptr;
zhac_configure_write_fn_t  g_cfg_write  = nullptr;
zhac_configure_sleep_fn_t  g_cfg_sleep  = nullptr;

// Per-call addressing (P2-T13, defs 3+4). The zhc library's bridge
// function-pointer types carry (device_index, ep, cluster, ...) as their
// first argument — they do NOT carry the device IEEE/NWK. The OLD design
// stashed the address in a pair of module globals (g_cfg_ieee/g_cfg_nwk)
// and serialised every configure + decode through a single mutex
// (g_cfg_addr_mtx), which the radio RX path (try_decode) blocked on
// portMAX_DELAY while a configure held it across multi-second Wait steps
// + bind/report radio round-trips — a multi-second frame-intake stall
// and a deadlock if a configure hook waited on a response the blocked RX
// task delivers.
//
// FIX: the slot table is append-only, so the library-supplied
// device_index uniquely and stably identifies the device for the life of
// the call. Each bridge resolves its OWN address from g_slots[idx]
// (ieee + cfg_nwk) — no shared globals, no cross-call mutex. A configure
// on one device and a decode on another use different device_index values
// and never collide, so try_decode takes NO lock for addressing at all.
struct BridgeAddr { std::uint64_t ieee; std::uint16_t nwk; bool ok; };
BridgeAddr bridge_addr_for(std::uint16_t idx) {
    SlotsLock lock;
    if (idx < g_slot_count) {
        return { g_slots[idx].ieee, g_slots[idx].cfg_nwk, true };
    }
    return { 0, 0, false };
}

bool configure_bind_bridge(std::uint16_t idx, std::uint8_t ep,
                            std::uint16_t cluster) {
    const auto a = bridge_addr_for(idx);
    return (g_cfg_bind && a.ok) ? g_cfg_bind(a.ieee, ep, cluster) : false;
}

bool configure_report_bridge(std::uint16_t idx, std::uint8_t ep,
                              std::uint16_t cluster, std::uint16_t attr,
                              std::uint8_t type, std::uint16_t mn,
                              std::uint16_t mx, std::uint32_t change,
                              std::uint16_t manu_code) {
    const auto a = bridge_addr_for(idx);
    return (g_cfg_report && a.ok)
        ? g_cfg_report(a.ieee, ep, cluster, attr,
                        type, mn, mx, change, manu_code)
        : false;
}

bool configure_cmd_bridge(std::uint16_t idx, std::uint8_t ep,
                           std::uint16_t cluster, std::uint8_t cmd,
                           const std::uint8_t* payload,
                           std::uint8_t payload_len,
                           std::uint8_t flags) {
    const auto a = bridge_addr_for(idx);
    return (g_cfg_cmd && a.ok)
        ? g_cfg_cmd(a.ieee, a.nwk, ep, cluster, cmd, payload,
                     payload_len, flags)
        : false;
}

bool configure_read_bridge(std::uint16_t idx, std::uint8_t ep,
                            std::uint16_t cluster,
                            const std::uint8_t* attr_ids_le,
                            std::uint8_t attr_count,
                            std::uint16_t manu_code) {
    const auto a = bridge_addr_for(idx);
    return (g_cfg_read && a.ok)
        ? g_cfg_read(a.ieee, a.nwk, ep, cluster, attr_ids_le,
                      attr_count, manu_code)
        : false;
}

bool configure_write_bridge(std::uint16_t idx, std::uint8_t ep,
                             std::uint16_t cluster, std::uint16_t attr,
                             std::uint8_t type, const std::uint8_t* value,
                             std::size_t len, std::uint16_t manu_code) {
    // The zhc library's ConfigureWriteFn carries the value length as
    // std::size_t, but a single ZCL attribute value is at most a few
    // bytes (the widest standard type is an 8-byte IEEE address / u64).
    // The bridge fn-ptr + radio transport take a uint8_t length, so clamp
    // here — guard against a malformed >255 spec rather than truncating
    // silently into a corrupt frame.
    if (len > 0xFF) return false;
    const auto a = bridge_addr_for(idx);
    return (g_cfg_write && a.ok)
        ? g_cfg_write(a.ieee, a.nwk, ep, cluster, attr, type, value,
                       static_cast<std::uint8_t>(len), manu_code)
        : false;
}

void configure_sleep_bridge(std::uint16_t wait_ms) {
    if (g_cfg_sleep) g_cfg_sleep(wait_ms);
}
}  // namespace

extern "C" void zhac_adapter_register_configure(
        zhac_configure_bind_fn_t   bind_fn,
        zhac_configure_report_fn_t report_fn) {
    g_cfg_bind   = bind_fn;
    g_cfg_report = report_fn;
}

extern "C" void zhac_adapter_register_configure_ex(
        zhac_configure_cmd_fn_t   cmd_fn,
        zhac_configure_read_fn_t  read_fn,
        zhac_configure_sleep_fn_t sleep_fn) {
    g_cfg_cmd   = cmd_fn;
    g_cfg_read  = read_fn;
    g_cfg_sleep = sleep_fn;
}

extern "C" void zhac_adapter_register_configure_write(
        zhac_configure_write_fn_t write_fn) {
    g_cfg_write = write_fn;
}

extern "C" bool zhac_adapter_configure(uint64_t ieee, uint16_t nwk,
                                        const char* model_id,
                                        const char* manu_name) {
    const zhc::PreparedDefinition* def =
        resolve_definition(ieee, model_id, manu_name);
    if (!def) return false;
    const zhc::PreparedDefinition* supp =
        resolve_supplement(ieee, model_id, manu_name, def);

    const std::uint16_t idx = resolve_device_index(ieee);
    if (idx == kInvalidDeviceIndex) {
        ESP_LOGE(TAG, "[configure] no slot for ieee=0x%016llx (table full)",
                  static_cast<unsigned long long>(ieee));
        return false;
    }
    // Latch this device's NWK into its own slot so the configure_* bridges
    // resolve the right destination by device_index. No cross-call globals,
    // no addr mutex — a parallel configure/decode on a DIFFERENT device
    // touches a different slot and cannot clobber this one. (def 3+4)
    {
        SlotsLock lock;
        if (idx < g_slot_count) g_slots[idx].cfg_nwk = nwk;
    }

    zhc::RuntimeContext ctx{};
    ctx.now_ms           = &now_ms;
    ctx.store            = &g_store;
    ctx.store_get        = &zhc::RuntimeStore<kMaxDevices>::get;
    ctx.timers           = &g_timer_scheduler;
    ctx.device_index     = idx;
    ctx.device_nwk       = nwk;
    ctx.configure_bind   = g_cfg_bind   ? &configure_bind_bridge   : nullptr;
    ctx.configure_report = g_cfg_report ? &configure_report_bridge : nullptr;
    ctx.configure_cmd    = g_cfg_cmd    ? &configure_cmd_bridge    : nullptr;
    ctx.configure_read   = g_cfg_read   ? &configure_read_bridge   : nullptr;
    ctx.configure_write  = g_cfg_write  ? &configure_write_bridge  : nullptr;
    ctx.configure_sleep  = g_cfg_sleep  ? &configure_sleep_bridge  : nullptr;

    bool ok = zhc::run_configure(*def, ctx);
    if (supp) {
        const bool supp_ok = zhc::run_configure(*supp, ctx);
        ESP_LOGI(TAG, "[configure] supplement ieee=0x%016llx %s",
                  static_cast<unsigned long long>(ieee),
                  supp_ok ? "ok" : "partial");
        ok = ok || supp_ok;
    }

    // Tuya MCU bootstrap. z2m's tuya.modernExtend.tuyaBase fires
    // dataQuery (cluster 0xEF00, cmd 0x03, empty payload) on configure
    // and on device-announce so the device dumps all DPs. Without it,
    // many sleepy sensors (ZTH05 etc.) join, sit silent, and never
    // report. Detect by manuSpecificTuya (0xEF00) cluster being bound.
    if (g_cfg_cmd) {
        bool has_tuya_cluster = false;
        std::uint8_t tuya_ep = 1;
        for (std::uint8_t i = 0; i < def->bindings_count; ++i) {
            if (def->bindings[i].cluster_id == 0xEF00) {
                has_tuya_cluster = true;
                if (def->bindings[i].endpoint != 0)
                    tuya_ep = def->bindings[i].endpoint;
                break;
            }
        }
        if (has_tuya_cluster) {
            (void)g_cfg_cmd(ieee, nwk, tuya_ep, 0xEF00,
                             0x03, nullptr, 0, 0);
            ESP_LOGI(TAG, "[configure] Tuya DATA_QUERY sent ieee=0x%016llx ep=%u",
                      static_cast<unsigned long long>(ieee), tuya_ep);
        }
    }

    return ok;
}

extern "C" void zhac_adapter_init(void) {
    merge_registries();
    ESP_LOGI(TAG,
             "zhc_adapter init — max_devices=%zu registry=%zu "
             "(see docs/PARITY_COVERAGE.md for per-vendor counts)",
             kMaxDevices, g_merged_count);

    // One-shot sanity: count how many def entries claim model "TS1002"
    // and print each so we can spot registry-order issues without
    // needing a host test.
    std::size_t ts1002_hits = 0;
    for (std::size_t i = 0; i < g_merged_count; ++i) {
        const zhc::PreparedDefinition* d = g_merged[i];
        if (!d || !d->zigbee_models) continue;
        for (std::uint8_t j = 0; j < d->zigbee_models_count; ++j) {
            if (d->zigbee_models[j] &&
                std::strcmp(d->zigbee_models[j], "TS1002") == 0) {
                ESP_LOGI(TAG, "  TS1002 def #%zu: %s/%s mfg_cnt=%u",
                         i,
                         d->vendor ? d->vendor : "?",
                         d->model  ? d->model  : "?",
                         d->manufacturer_names_count);
                ++ts1002_hits;
                break;
            }
        }
    }
    ESP_LOGI(TAG, "  TS1002 total matches in merged registry: %zu",
             ts1002_hits);
}

extern "C" bool zhac_adapter_has_def(uint64_t ieee,
                                      const char* model_id,
                                      const char* manufacturer_name) {
    if (!model_id || !model_id[0]) return false;
    if (find_definition(model_id, manufacturer_name) != nullptr) return true;
    if (ieee == 0) return false;
    return zhc_fallback::synth_definition(ieee, model_id, manufacturer_name)
            != nullptr;
}

extern "C" uint8_t zhac_adapter_power_source_override(const char* model_id,
                                                       const char* manufacturer_name) {
    if (!model_id || !model_id[0]) return 0;
    const zhc::PreparedDefinition* def =
        find_definition(model_id, manufacturer_name);
    return def ? def->power_source_override : 0;
}

extern "C" void zhac_adapter_register_endpoint(uint64_t ieee,
                                                uint8_t  endpoint,
                                                uint16_t profile_id,
                                                uint16_t device_id,
                                                const uint16_t* in_clusters,
                                                size_t   n_in_clusters,
                                                const uint16_t* out_clusters,
                                                size_t   n_out_clusters) {
    zhc_fallback::register_endpoint(ieee, endpoint, profile_id, device_id,
                                     in_clusters, n_in_clusters,
                                     out_clusters, n_out_clusters);
    // New cluster data could promote a device from UNMATCHED to MATCHED
    // via synth; drop any cached def (including the negative-miss
    // sentinel) so the next try_decode re-runs resolution. Also drop the
    // supplement cache because the added clusters may change what the
    // supplement emits. This is the epoch-invariant pairing the fallback
    // header documents (rebuild trigger ⇔ cached_def invalidation).
    clear_cached_defs_for(ieee);
}

extern "C" void zhac_adapter_fallback_clear(uint64_t ieee) {
    zhc_fallback::clear(ieee);
    clear_cached_defs_for(ieee);
}

extern "C" void zhac_adapter_invalidate_def_cache(uint64_t ieee) {
    clear_cached_defs_for(ieee);
    // The synthesized fallback def is rebuilt on demand from cluster
    // data still cached in zhc_adapter_fallback; no extra clear needed
    // here unless the caller also wants the cluster data gone (use
    // zhac_adapter_fallback_clear for that).
}

namespace zhc_adapter_internal {

// Called by the fallback pool (which holds its own pool mutex) right
// before a victim slot's built-def storage is repurposed for a
// different device (LRU eviction) or dropped (clear). Walks every
// IeeeSlot and clears cached pointers that land inside the victim's
// storage range — without this, the evicted device's frames would
// silently keep decoding through storage that, after the next rebuild,
// describes the NEW device (cross-device state corruption).
//
// LOCK ORDER (P2-T13): this walk runs UNDER g_pool_mtx (the fallback
// pool holds it). It deliberately does NOT take s_slots_mtx. The slot
// resolve path takes s_slots_mtx and later (sequentially, never nested)
// takes g_pool_mtx for synth/owns(); taking s_slots_mtx here would invert
// that order (g_pool_mtx → s_slots_mtx) and could deadlock. We are safe
// without it because:
//  * The slot table is APPEND-ONLY — entries are never moved, so a
//    concurrent resolve_device_index append can only add a slot whose
//    cached_def is still nullptr (cannot point into the victim's range).
//  * `g_slot_count` only grows; reading it racily here can under-count by
//    at most the in-flight append, which (per the point above) holds no
//    pointer into the victim yet — so nothing is missed.
//  * cached_def / cached_supplement are single aligned pointers —
//    word-atomic load/store on both 32-bit targets (the codebase's
//    existing relaxed-reader pattern).
// Two residual races, both handled:
// (a) BENIGN — a decode on another core re-reads a pointer we are
// about to clear and dispatches one last frame against the still-
// intact OLD bytes, bounded by the fallback's A/B buffer keeping the
// victim's published half byte-stable for one more generation.
// (b) NOT benign if left open — a decode that resolved the victim's def
// just before the eviction took the pool mutex could re-cache it just
// after this walk. Such a stale entry would survive the repurposed
// slot's FIRST rebuild (which writes the other half), but the slot's
// SECOND rebuild rewrites the very bytes it points at — the old device's
// frames would then decode through the NEW device's def. The decode-miss
// path therefore re-validates every cache fill against the pool under its
// mutex (zhc_fallback::owns) and drops the fill if the slot no longer
// belongs to that ieee, closing (b).
void invalidate_cached_defs_in(const void* begin, const void* end) {
    const auto lo = reinterpret_cast<std::uintptr_t>(begin);
    const auto hi = reinterpret_cast<std::uintptr_t>(end);
    for (std::size_t i = 0; i < g_slot_count; ++i) {
        auto& s = g_slots[i];
        const auto d = reinterpret_cast<std::uintptr_t>(s.cached_def);
        if (d >= lo && d < hi) s.cached_def = nullptr;
        const auto p = reinterpret_cast<std::uintptr_t>(s.cached_supplement);
        if (p >= lo && p < hi) s.cached_supplement = nullptr;
    }
}

}  // namespace zhc_adapter_internal

extern "C" bool zhac_adapter_resolve_labels(const char* model_id,
                                              const char* manufacturer_name,
                                              char* vendor_out, size_t vendor_cap,
                                              char* model_out,  size_t model_cap) {
    if (!model_id || !model_id[0]) return false;
    const zhc::PreparedDefinition* def =
        find_definition(model_id, manufacturer_name);
    if (!def) return false;
    if (vendor_out && vendor_cap > 0) {
        const char* v = def->vendor ? def->vendor : "";
        std::strncpy(vendor_out, v, vendor_cap - 1);
        vendor_out[vendor_cap - 1] = '\0';
    }
    if (model_out && model_cap > 0) {
        const char* m = def->model ? def->model : "";
        std::strncpy(model_out, m, model_cap - 1);
        model_out[model_cap - 1] = '\0';
    }
    return true;
}

// Serialize the matching device's `exposes` as a compact JSON array.
// Returns bytes written (excluding NUL) or 0 on miss / buffer
// overflow. Used by `hap_json_encode_device_info` to surface
// `exposes` on the wire so the UI can drive read-only / writable /
// enum classification from the library, not a hardcoded allowlist.
extern "C" size_t zhac_adapter_build_exposes_json(uint64_t ieee,
                                                    const char* model_id,
                                                    const char* manufacturer_name,
                                                    char* buf, size_t cap) {
    if (!buf || cap < 3) return 0;
    const zhc::PreparedDefinition* def =
        resolve_definition(ieee, model_id, manufacturer_name);
    const zhc::PreparedDefinition* supp =
        resolve_supplement(ieee, model_id, manufacturer_name, def);
    const bool have_primary    = def  && def->exposes  && def->exposes_count > 0;
    const bool have_supplement = supp && supp->exposes && supp->exposes_count > 0;
    if (!have_primary && !have_supplement) {
        // Still emit a valid empty array so the UI has a consistent
        // shape (`[]`) instead of `undefined`.
        if (cap < 3) return 0;
        buf[0] = '['; buf[1] = ']'; buf[2] = '\0';
        return 2;
    }
    auto type_str = [](zhc::ExposeType t) -> const char* {
        switch (t) {
            case zhc::ExposeType::Numeric: return "numeric";
            case zhc::ExposeType::Enum:    return "enum";
            case zhc::ExposeType::Binary:  return "binary";
            case zhc::ExposeType::String:  return "text";
        }
        return "?";
    };
    // z2m access bitmask: bit0=STATE (publishes), bit1=SET (writable),
    // bit2=GET (explicit read). Mirror the three Access enum cases:
    auto access_bits = [](zhc::Access a) -> unsigned {
        switch (a) {
            case zhc::Access::State:    return 0b001;
            case zhc::Access::Set:      return 0b010;
            case zhc::Access::StateSet: return 0b011;
        }
        return 0;
    };
    size_t pos = 0;
    auto write = [&](const char* s) {
        while (*s && pos + 1 < cap) buf[pos++] = *s++;
    };
    auto write_quoted = [&](const char* s) {
        if (pos + 1 >= cap) return;
        buf[pos++] = '"';
        for (const char* p = s; *p && pos + 2 < cap; p++) {
            unsigned char c = (unsigned char)*p;
            if (c == '"' || c == '\\') { if (pos + 3 >= cap) break; buf[pos++] = '\\'; buf[pos++] = (char)c; }
            else if (c < 0x20)          { /* drop control chars */ }
            else                         { buf[pos++] = (char)c; }
        }
        if (pos + 1 < cap) buf[pos++] = '"';
    };
    auto write_num = [&](unsigned n) {
        char tmp[8];
        int w = snprintf(tmp, sizeof(tmp), "%u", n);
        if (w > 0) write(tmp);
    };
    auto write_int = [&](std::int32_t n) {   // signed — numeric bounds may be negative
        char tmp[12];
        int w = snprintf(tmp, sizeof(tmp), "%d", static_cast<int>(n));
        if (w > 0) write(tmp);
    };

    auto category_str = [](zhc::ExposeCategory c) -> const char* {
        switch (c) {
            case zhc::ExposeCategory::State:      return nullptr;   // default, omit
            case zhc::ExposeCategory::Config:     return "config";
            case zhc::ExposeCategory::Diagnostic: return "diagnostic";
        }
        return nullptr;
    };

    size_t emitted = 0;
    auto emit_one = [&](const zhc::Expose& e) {
        if (emitted++) write(",");
        write("{\"name\":");
        write_quoted(e.name ? e.name : "");
        write(",\"type\":");
        write_quoted(type_str(e.type));
        write(",\"access\":");
        write_num(access_bits(e.access));
        if (e.unit && e.unit[0]) {
            write(",\"unit\":");
            write_quoted(e.unit);
        }
        if (const char* cat = category_str(e.category)) {
            write(",\"category\":");
            write_quoted(cat);
        }
        if (e.enum_values && e.enum_count > 0) {
            write(",\"values\":[");
            for (std::uint8_t j = 0; j < e.enum_count; j++) {
                if (j) write(",");
                write_quoted(e.enum_values[j] ? e.enum_values[j] : "");
            }
            write("]");
        }
        // Numeric bounds (UI slider scaling). Prefer the def's explicit bounds; else fill the
        // standard attrs (brightness/color_temp/position/…) from the central range table so a
        // slider scales correctly without per-def edits. Emitted as z2m's value_min/value_max.
        if (e.type == zhc::ExposeType::Numeric) {
            std::int32_t lo = e.value_min, hi = e.value_max, st = e.value_step;
            if (hi <= lo) zhc::default_numeric_range(e.name, lo, hi, st);
            if (hi > lo) {
                write(",\"value_min\":"); write_int(lo);
                write(",\"value_max\":"); write_int(hi);
                if (st > 0) { write(",\"value_step\":"); write_int(st); }
            }
        }
        write("}");
    };
    auto already_emitted_name = [&](const char* name,
                                     const zhc::PreparedDefinition* earlier) -> bool {
        if (!earlier || !earlier->exposes || !name) return false;
        for (std::uint8_t i = 0; i < earlier->exposes_count; i++) {
            const char* n = earlier->exposes[i].name;
            if (n && std::strcmp(n, name) == 0) return true;
        }
        return false;
    };

    write("[");
    if (have_primary) {
        for (std::uint8_t i = 0; i < def->exposes_count; i++) {
            emit_one(def->exposes[i]);
        }
    }
    if (have_supplement) {
        // Drop names already emitted by the primary def so the UI
        // doesn't show duplicate "state"/"battery"/etc. rows when a
        // DIY fork extends a registry-matched device.
        for (std::uint8_t i = 0; i < supp->exposes_count; i++) {
            const auto& e = supp->exposes[i];
            if (already_emitted_name(e.name, def)) continue;
            emit_one(e);
        }
    }
    write("]");
    if (pos + 1 >= cap) return 0;   // truncated
    buf[pos] = '\0';
    return pos;
}

extern "C" bool zhac_adapter_try_decode(uint64_t ieee,
                                         const char* model_id,
                                         const char* manufacturer_name,
                                         uint16_t group_id,
                                         uint16_t cluster_id,
                                         uint8_t src_endpoint,
                                         uint8_t linkquality,
                                         const uint8_t* zcl,
                                         size_t zcl_len) {
    _METRIC_TIMER_SCOPE(METRIC_ZHC_DECODE);
    _METRIC_COUNTER_INC(METRIC_ZB_RX_FRAMES_TOTAL, 1);

    // Fast path: per-ieee def cache (snapshotted under s_slots_mtx so the
    // radio task never dereferences g_slots without the lock). Fills on
    // first frame per device from find_definition (which walks 5500+
    // entries); subsequent frames dereference the cached pointer directly.
    // See `zhac_adapter_invalidate_def_cache` for the eviction path.
    SlotSnapshot snap = snapshot_slot(ieee);
    const zhc::PreparedDefinition* def  = snap.present ? snap.cached_def : nullptr;
    const zhc::PreparedDefinition* supp = snap.present ? snap.cached_supplement : nullptr;

    // Negative-miss cache (def 5): a device we resolved and that matched
    // NOTHING is tagged with kMissSentinel so its frames don't re-walk the
    // 5500-def registry (and re-log) every time. Cleared on register_
    // endpoint / fallback_clear / invalidate_def_cache, so a device that
    // later gains cluster data and matches via synth is NOT masked.
    if (def == kMissSentinel) {
        _METRIC_COUNTER_INC(METRIC_ADAPTER_CACHE_HIT, 1);
        _METRIC_COUNTER_INC(METRIC_ZHC_UNHANDLED_TOTAL, 1);
        return false;
    }

    if (def) {
        _METRIC_COUNTER_INC(METRIC_ADAPTER_CACHE_HIT, 1);
    } else {
        _METRIC_COUNTER_INC(METRIC_ADAPTER_CACHE_MISS, 1);
        def  = resolve_definition(ieee, model_id, manufacturer_name);
        supp = resolve_supplement(ieee, model_id, manufacturer_name, def);
        // Demoted to DEBUG (def 5): this fired at INFO on EVERY frame of an
        // unmatched device on the radio task. With the miss cache below it
        // now logs at most once per device until invalidation, and the
        // routine resolve detail is debug-level.
        ESP_LOGD(TAG,
                 "[zhc-lib] def resolve ieee=0x%016llx model='%s' mfg='%s' "
                 "-> %s/%s%s (reg=%zu, cached)",
                 static_cast<unsigned long long>(ieee),
                 model_id        ? model_id        : "(null)",
                 manufacturer_name ? manufacturer_name : "(null)",
                 def && def->vendor ? def->vendor : "-",
                 def && def->model  ? def->model  : "-",
                 supp ? " +fallback" : "",
                 g_merged_count);
        if (!snap.present) {
            (void)resolve_device_index(ieee);
            snap = snapshot_slot(ieee);
        }
        if (snap.present && def) {
            store_cached_defs(ieee, def, supp);
            // Race (b) closure (see invalidate_cached_defs_in): the
            // resolve above ran without the fallback pool mutex, so a
            // pool-backed def may belong to a slot that was evicted /
            // cleared / repurposed between resolve and the stores —
            // and the invalidation walk may have run BEFORE the stores
            // landed, missing them. Store first, then re-validate
            // ownership under the pool mutex: either the walk saw the
            // entries and cleared them, or owns() sees the slot moved
            // on and we drop the fill here. Either way no stale pool
            // pointer persists. MISS path only — the cached-hit fast
            // path above never takes the pool lock. The local def/supp
            // still dispatch this one frame below, against bytes the
            // A/B buffer keeps frozen for one generation (same shape
            // as benign race (a)).
            if (!zhc_fallback::owns(ieee, def) ||
                !zhc_fallback::owns(ieee, supp)) {
                store_cached_defs(ieee, nullptr, nullptr);
            }
        } else if (snap.present && !def) {
            // Resolved, no match → tag the slot so we don't re-walk every
            // frame. kMissSentinel is never dereferenced (guard above).
            store_cached_defs(ieee, kMissSentinel, nullptr);
        }
    }

    if (!def) {
        _METRIC_COUNTER_INC(METRIC_ZHC_UNHANDLED_TOTAL, 1);
        return false;           // device not yet ported — skip silently
    }
    // Slot-table overflow (def 1): we have a def but no slot (table full at
    // ZAP_MAX_DEVICES). FAIL the decode rather than dispatch with an
    // invalid device_index — never alias onto slot 0 and corrupt another
    // device's converter state. resolve_device_index already logged once.
    if (!snap.present || snap.idx == kInvalidDeviceIndex) {
        _METRIC_COUNTER_INC(METRIC_ZHC_UNHANDLED_TOTAL, 1);
        return false;
    }
    if (!zcl || zcl_len == 0) return false;

    zhc::InboundApsFrame raw{};
    raw.cluster_id   = cluster_id;
    raw.src_addr     = 0;
    raw.group_id     = group_id;     // 0 for unicasts; non-0 for groupcasts
    raw.src_endpoint = src_endpoint;
    raw.dst_endpoint = 1;
    raw.linkquality  = linkquality;
    raw.data         = std::span<const std::uint8_t>(zcl, zcl_len);

    zhc::DecodedMessage msg{};
    if (!zhc::decode_frame(raw, {}, msg)) {
        ESP_LOGW(TAG, "[zhc-lib] 0x%016llx decode_frame failed cluster=0x%04x",
                 static_cast<unsigned long long>(ieee), cluster_id);
        return false;
    }
    msg.cluster = cluster_id_to_name(cluster_id);

    // If this is a Tuya DP-stream frame, parse the on-wire record list
    // so the definition's TuyaDp-family converters see real DPs.
    zhc::TuyaDpRecord dp_scratch[16];
    std::size_t dp_count = 0;
    std::span<const zhc::TuyaDpRecord> dp_span{};
    if (msg.family == zhc::FrameFamily::TuyaDp) {
        zhc::ZclHeader hdr{}; std::size_t hlen = 0;
        if (zhc::parse_zcl_header(raw.data, hdr, hlen) &&
            raw.data.size() >= hlen + 2) {
            const auto body = raw.data.subspan(hlen + 2);  // skip seq
            zhc::parse_tuya_dp_stream(body,
                std::span<zhc::TuyaDpRecord>(dp_scratch,
                    sizeof(dp_scratch) / sizeof(dp_scratch[0])),
                dp_count);
            dp_span = std::span<const zhc::TuyaDpRecord>(dp_scratch, dp_count);
        }
    }

    zhc::RuntimeContext ctx{};
    ctx.now_ms       = &now_ms;
    ctx.store        = &g_store;
    ctx.store_get    = &zhc::RuntimeStore<kMaxDevices>::get;
    ctx.timers       = &g_timer_scheduler;
    ctx.device_index = snap.idx;
    // Per-call addressing (def 3+4) — NO mutex on the RX path. Some fz
    // converters (Zosung IR runtime, Tuya MCU sync-time) emit ZCL commands
    // back to the device while handling an inbound frame, via the reused
    // configure_* hooks (a generic "send a ZCL frame" interface). Those
    // bridges resolve the destination from g_slots[device_index] — the
    // slot this very frame's ieee owns — so there is NOTHING shared with a
    // parallel configure to protect. ctx.device_nwk (used directly by the
    // Tuya kick below + the library's Callback op) comes from the same
    // slot, latched by zhac_adapter_set_runtime_addr before this call.
    // This removes the old g_cfg_addr_mtx that the RX task blocked on
    // portMAX_DELAY across multi-second configure Wait steps (the stall +
    // deadlock this fix targets).
    ctx.device_nwk = snap.cfg_nwk;
    ctx.configure_bind   = g_cfg_bind   ? &configure_bind_bridge   : nullptr;
    ctx.configure_report = g_cfg_report ? &configure_report_bridge : nullptr;
    ctx.configure_cmd    = g_cfg_cmd    ? &configure_cmd_bridge    : nullptr;
    ctx.configure_read   = g_cfg_read   ? &configure_read_bridge   : nullptr;
    ctx.configure_write  = g_cfg_write  ? &configure_write_bridge  : nullptr;

    auto result = zhc::dispatch_from_zigbee(msg, dp_span, *def, raw, ctx);

    // If the primary (registry) def didn't claim the frame but we've
    // built a supplementary fallback for extra clusters (e.g. a DIY
    // fork that added temp/humidity to a vanilla range extender), try
    // the fallback def against the same frame. Keeps the registry
    // match authoritative while covering z2m-unknown clusters.
    if (!result.any_matched && supp) {
        result = zhc::dispatch_from_zigbee(msg, dp_span, *supp, raw, ctx);
        if (result.any_matched) {
            log_payload(ieee, *supp, result, cluster_id, &msg, zcl, zcl_len);
            fire_shadow_updates(ieee, result);
            return true;
        }
    }

    // Tuya MCU bootstrap kick: when an unmatched mcuVersionResponse
    // arrives (cluster 0xEF00 cmd 0x11), z2m's tuya.modernExtend
    // `respondToMcuVersionResponse` answers with a dataQuery so the
    // device transitions out of handshake into active DP reporting.
    // Without this many sleepy Tuya sensors (ZTH05 etc.) join, send
    // their version banner, and then go silent forever.
    if (!result.any_matched && cluster_id == 0xEF00 &&
        msg.family == zhc::FrameFamily::Zcl &&
        msg.command_id == 0x11 && g_cfg_cmd) {
        (void)g_cfg_cmd(ieee, ctx.device_nwk, 1, 0xEF00, 0x03, nullptr, 0, 0);
        ESP_LOGI(TAG, "[zhc-lib] mcuVersionResp seen ieee=0x%016llx — sent dataQuery",
                 static_cast<unsigned long long>(ieee));
    }

    log_payload(ieee, *def, result, cluster_id, &msg, zcl, zcl_len);
    if (result.any_matched) fire_shadow_updates(ieee, result);
    return result.any_matched;
}

// ── Send path ────────────────────────────────────────────────────────

namespace {
zhac_af_send_fn_t g_af_send = nullptr;

bool dispatch_and_send(uint64_t ieee,
                        const char* model_id, const char* manu_name,
                        uint16_t nwk_addr, uint8_t dst_ep,
                        const char* key, const zhc::Value& value) {
    if (!model_id || !key) return false;

    // Reuse the per-ieee cached def that try_decode maintains (def 6)
    // instead of re-walking the 5500-def registry (or rebuilding a
    // fallback) on every command. Snapshot under s_slots_mtx; the cached
    // pointer is honoured only if it is a real def (not null, not the
    // negative-miss sentinel) AND still owned by this ieee in the pool
    // (the one-generation A/B lifetime rule — revalidate via owns() under
    // g_pool_mtx, sequential with the slot lock, never nested). Otherwise
    // fall back to a fresh resolve. We do NOT write the cache here — that
    // stays try_decode's job, keeping the eviction/invalidation contract
    // in one place.
    const SlotSnapshot snap = snapshot_slot(ieee);
    const zhc::PreparedDefinition* def = nullptr;
    if (snap.present && snap.cached_def && snap.cached_def != kMissSentinel &&
        zhc_fallback::owns(ieee, snap.cached_def)) {
        def = snap.cached_def;
    } else {
        def = resolve_definition(ieee, model_id, manu_name);
    }
    if (!def) {
        ESP_LOGW(TAG, "[zhc-send] 0x%016llx model=%s no definition",
                 static_cast<unsigned long long>(ieee), model_id);
        return false;
    }
    if (!g_af_send) {
        ESP_LOGW(TAG, "[zhc-send] 0x%016llx radio hook not registered",
                 static_cast<unsigned long long>(ieee));
        return false;
    }

    zhc::RuntimeContext ctx{};
    ctx.now_ms       = &now_ms;
    ctx.store        = &g_store;
    ctx.store_get    = &zhc::RuntimeStore<kMaxDevices>::get;
    ctx.timers       = &g_timer_scheduler;
    ctx.device_index = resolve_device_index(ieee);
    ctx.device_nwk   = nwk_addr;

    // Multi-endpoint Tz routing. If the def opts in via endpoint_map
    // and `key` ends with `_<label>` matching a registered endpoint,
    // strip the suffix and route the outbound frame to that endpoint
    // instead of the caller-supplied `dst_ep`. Single-EP devices
    // (default null map) keep their original key + dst_ep.
    std::string_view dispatch_key(key);
    std::uint8_t     target_ep = dst_ep;
    bool             suffix_matched = false;
    if (def->endpoint_map && def->endpoint_map_count > 0) {
        const auto pos = dispatch_key.find_last_of('_');
        if (pos != std::string_view::npos) {
            const std::string_view suffix = dispatch_key.substr(pos + 1);
            for (std::uint8_t i = 0; i < def->endpoint_map_count; ++i) {
                const auto& lbl = def->endpoint_map[i];
                const std::size_t lbl_len = std::strlen(lbl.label);
                if (suffix.size() == lbl_len &&
                    std::memcmp(suffix.data(), lbl.label, lbl_len) == 0) {
                    dispatch_key   = dispatch_key.substr(0, pos);
                    target_ep      = lbl.endpoint;
                    suffix_matched = true;
                    break;
                }
            }
        }
    }
    // No endpoint_map suffix matched but the def declared a non-1
    // default endpoint (z2m's `endpoint:{default:N}` shape) — route
    // to that endpoint instead of the caller-supplied dst_ep.
    if (!suffix_matched && def->default_endpoint != 0) {
        target_ep = def->default_endpoint;
    }

    std::uint8_t frame[64] = {};
    auto r = zhc::dispatch_to_zigbee(*def, dispatch_key, value, ctx,
        std::span<std::uint8_t>(frame, sizeof(frame)));
    if (!r.ok) {
        // Registry def's TZ list doesn't claim this key — fall through
        // to the supplementary fallback (covers DIY forks that add
        // writable clusters beyond what the registry def knows).
        if (const auto* supp =
                resolve_supplement(ieee, model_id, manu_name, def)) {
            r = zhc::dispatch_to_zigbee(*supp, dispatch_key, value, ctx,
                std::span<std::uint8_t>(frame, sizeof(frame)));
        }
    }
    if (!r.ok) {
        ESP_LOGW(TAG, "[zhc-send] 0x%016llx key='%s' no converter / encode failed",
                 static_cast<unsigned long long>(ieee), key);
        return false;
    }

    return g_af_send(nwk_addr, target_ep, r.cluster_id,
                      frame, r.frame_size);
}
}  // namespace

extern "C" void zhac_adapter_register_send(zhac_af_send_fn_t fn) {
    g_af_send = fn;
}

extern "C" void zhac_adapter_set_runtime_addr(uint64_t ieee, uint16_t nwk) {
    // Per-call addressing (def 3+4): latch this device's current NWK into
    // ITS OWN slot (keyed on the stable ieee), so the converter-response
    // bridges (Tuya sync-time etc.) resolve the right destination by
    // device_index during the subsequent try_decode. No global address,
    // no cross-device clobber — a different device's set_runtime_addr or a
    // configure writes a DIFFERENT slot. Allocates the slot if first-seen
    // so the very first frame's response routes correctly.
    const std::uint16_t idx = resolve_device_index(ieee);
    if (idx == kInvalidDeviceIndex) return;   // table full — logged once
    SlotsLock lock;
    if (idx < g_slot_count) g_slots[idx].cfg_nwk = nwk;
}

extern "C" bool zhac_adapter_send_bool(uint64_t ieee,
                                        const char* model_id,
                                        const char* manu_name,
                                        uint16_t nwk_addr, uint8_t dst_ep,
                                        const char* key, bool value) {
    zhc::Value v{}; v.type = zhc::ValueType::Bool; v.b = value;
    return dispatch_and_send(ieee, model_id, manu_name, nwk_addr, dst_ep,
                              key, v);
}

extern "C" bool zhac_adapter_send_uint(uint64_t ieee,
                                        const char* model_id,
                                        const char* manu_name,
                                        uint16_t nwk_addr, uint8_t dst_ep,
                                        const char* key, uint64_t value) {
    zhc::Value v{}; v.type = zhc::ValueType::Uint; v.u = value;
    return dispatch_and_send(ieee, model_id, manu_name, nwk_addr, dst_ep,
                              key, v);
}

extern "C" bool zhac_adapter_send_string(uint64_t ieee,
                                          const char* model_id,
                                          const char* manu_name,
                                          uint16_t nwk_addr, uint8_t dst_ep,
                                          const char* key,
                                          const char* value) {
    zhc::Value v{}; v.type = zhc::ValueType::StringRef; v.str = value;
    return dispatch_and_send(ieee, model_id, manu_name, nwk_addr, dst_ep,
                              key, v);
}
