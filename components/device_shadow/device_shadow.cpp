// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "device_shadow.h"
#include "event_bus.h"
#include "zap_store.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"   // EXT_RAM_BSS_ATTR — park the big CRC scratch in PSRAM
#include "esp_log.h"
#include "metrics/metrics_macros.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "nvs_checked.h"
#include "esp_rom_crc.h"   // F26: esp_rom_crc32_le for blob integrity
#include <cstring>
#include <cstdio>
#include <ctime>
#include "task_stacks.h"

static const char* TAG = "device_shadow";
static const char* NVS_NS = "zap_shadow";
static constexpr uint32_t NVS_MIN_INTERVAL_S = 300; // max one NVS write per 5min per device
// task_shadow cadence: the queue-drain timeout AND the housekeeping/
// persistence sweep period. Comments below cite this constant by name.
static constexpr uint32_t SWEEP_PERIOD_MS = 100;
// Depth of s_task_queue (timer-cb → task_shadow messages). Overflow is safe:
// both timer callbacks fall back to per-entry pending flags the sweep drains.
static constexpr UBaseType_t TASK_QUEUE_DEPTH = 16;

// Canonical internal attribute names.
static constexpr const char* KEY_OCCUPANCY = "occupancy";
static constexpr const char* KEY_LAST_SEEN = "_last_seen";

// Cached NVS handle — opened once in init, kept open for firmware lifetime.
static nvs_handle_t s_nvs = 0;

// F26 (FINDINGS.md): the attr blob is stored as [ShadowBlobHdr][ShadowAttr×count]
// with a version byte + count + CRC32 over the payload, so a torn / wrong-length /
// corrupt blob is rejected on load instead of being silently accepted as a
// truncated final attr. SHADOW_ATTR_MAX bounds both the on-disk count and the
// shared scratch buffer below.
static constexpr uint8_t SHADOW_ATTR_BLOB_VER = 1;
static constexpr uint8_t SHADOW_ATTR_MAX      = 32;   // == DeviceShadowEntry::attrs[32]
struct __attribute__((packed)) ShadowBlobHdr {
    uint8_t  ver;     // SHADOW_ATTR_BLOB_VER
    uint8_t  count;   // number of ShadowAttr records following
    uint16_t _pad;
    uint32_t crc;     // esp_rom_crc32_le(0, payload, count*sizeof(ShadowAttr))
};
// LOAD-only scratch: nvs_load_attrs() fills it during boot restore, which is
// documented single-call/single-task and runs OUTSIDE s_mutex (leaf-lock rule:
// no nvs_* under the shadow lock). The attr SAVE path owns a separate scratch
// (s_sweep_blob, task_shadow-exclusive), so a boot-time load can never race an
// in-flight sweep write. Parked in PSRAM (EXT_RAM_BSS_ATTR): ~2.7 KB is too
// much for the tight internal DRAM of the single-chip (mono) build, which it
// overflowed by ~1.3 KB. Never touched from an ISR, so external RAM is fine.
// No-op (stays internal) on targets without PSRAM-BSS enabled.
EXT_RAM_BSS_ATTR static uint8_t s_attr_blob[sizeof(ShadowBlobHdr) + SHADOW_ATTR_MAX * sizeof(ShadowAttr)];

// Q76 (QWEN_FINDINGS triage): NVS keys cap at 15 chars, so the original
// "a%014llX" packed only 56 of the 64 IEEE bits — two devices differing only in
// the top MAC byte (e.g. different vendors) collided on a single shadow record.
// base36 packs the full 64-bit IEEE into ≤13 chars (+1 prefix ≤ 15). The v6→v7
// NVS bump (below) wipes the old 56-bit-keyed entries, so there's no migration.
static void shadow_key(char out[16], char prefix, uint64_t ieee) {
    out[0] = prefix;
    char tmp[16];
    int n = 0;
    do {
        unsigned d = (unsigned)(ieee % 36u);
        ieee /= 36u;
        tmp[n++] = (d < 10) ? (char)('0' + d) : (char)('a' + d - 10);
    } while (ieee);
    int w = 1;
    while (n > 0) out[w++] = tmp[--n];
    out[w] = '\0';
}

// ── Pipeline helpers (defined in shadow_pipeline.cpp) ────────────────────
extern "C" uint8_t shadow_pipeline_filter(const DeviceConfig*, const ZclAttribute*, uint8_t,
                                            ZclAttribute*, uint8_t);
extern "C" bool    shadow_pipeline_throttle_pass(DeviceConfig*, uint32_t*, uint32_t);
extern "C" int8_t  shadow_pipeline_debounce_bypass(const DeviceConfig*, const PendingState*,
                                                     const ZclAttribute*);
extern "C" void    shadow_pipeline_merge_pending(const DeviceConfig*, PendingState*,
                                                  const ZclAttribute*, uint8_t);
extern "C" uint8_t shadow_pipeline_flush_pending(PendingState*, ZclAttribute*, uint8_t);

// ── Shadow table (allocated in PSRAM on init) ─────────────────────────────
static DeviceShadowEntry* s_shadow = nullptr;
static uint16_t           s_count  = 0;

// Lock discipline (P1 lock/NVS decoupling — pre-fix refs :412/:446/:500/:334/
// :248):
//   s_mutex      — LEAF lock over the table. Nothing else is ever acquired
//                  inside it: no nvs_* (flash writes took tens-hundreds of ms
//                  on the radio RX path and across the 200-entry sweep), no
//                  event_bus_publish (bus snapshots under ITS lock and runs
//                  filters in the publisher's task — shadow→bus nesting let
//                  any filter touching the shadow API deadlock), no blocking
//                  xTimer ops (0-block-time posts are fine).
//   s_emit_mutex — serialises every ZCL_ATTR-emitting path and is ALWAYS
//                  taken BEFORE s_mutex. It owns the shared s_staged buffer
//                  and is held across publish_staged(), so events are
//                  published after s_mutex is released yet still in exactly
//                  the order their state mutations committed (parity with the
//                  old publish-under-s_mutex total order). Read-only API
//                  (get_attrs/get_config) takes s_mutex alone and is no
//                  longer stalled by publishes or flash writes.
// Bus subscribers/filters may call read-only shadow API; calling an emitting
// shadow API from a filter self-deadlocks on s_emit_mutex — same constraint
// the old code had on s_mutex, just on the outer lock now.
static SemaphoreHandle_t s_mutex;
static SemaphoreHandle_t s_emit_mutex;

// One housekeeping queue for both timer-callback kinds. Timer callbacks run
// on the SHARED FreeRTOS timer service task and must never block on s_mutex
// (a stalled shadow lock would freeze every software timer in the firmware) —
// they do a zero-timeout enqueue and task_shadow does the locked work.
enum class ShadowMsgKind : uint8_t { DebounceFlush, OccupancyTimeout };
struct ShadowTaskMsg { uint64_t ieee; ShadowMsgKind kind; };
static QueueHandle_t s_task_queue;

static DeviceShadowEntry* find_entry(uint64_t ieee);   // fwd — used by persist helpers

// ── NVS helpers ───────────────────────────────────────────────────────────

// Serialize [ShadowBlobHdr][attrs] into `buf` (capacity must be at least
// sizeof(ShadowBlobHdr) + SHADOW_ATTR_MAX*sizeof(ShadowAttr)). Pure RAM work
// — safe under s_mutex. F26: prepends version + count + CRC header (see
// ShadowBlobHdr). Returns the total blob length.
static size_t attr_blob_serialize(uint8_t* buf, const ShadowAttr* attrs, uint8_t count) {
    if (count > SHADOW_ATTR_MAX) count = SHADOW_ATTR_MAX;
    const size_t plen = (size_t)count * sizeof(ShadowAttr);
    auto* h   = reinterpret_cast<ShadowBlobHdr*>(buf);
    auto* pay = buf + sizeof(ShadowBlobHdr);
    memcpy(pay, attrs, plen);
    h->ver   = SHADOW_ATTR_BLOB_VER;
    h->count = count;
    h->_pad  = 0;
    h->crc   = esp_rom_crc32_le(0, pay, plen);
    return sizeof(ShadowBlobHdr) + plen;
}

// Flash half — tens to hundreds of ms incl. page erase, so it must run with
// s_mutex RELEASED (the pre-fix code committed under the lock from the radio
// RX path at :412 and across the whole sweep at :446, stalling readers, the
// radio path and — via the old occupancy callback — the FreeRTOS timer task).
static bool nvs_write_attr_blob(uint64_t ieee, const uint8_t* blob, size_t len) {
    if (!s_nvs) return false;
    char key[16];
    shadow_key(key, 'a', ieee);   // Q76: full 64-bit IEEE, base36
    esp_err_t err = nvs_set_blob(s_nvs, key, blob, len);
    if (err != ESP_OK) { ESP_LOGE(TAG, "nvs_set_blob attrs: %s", esp_err_to_name(err)); return false; }
    err = nvs_commit(s_nvs);
    if (err != ESP_OK) ESP_LOGE(TAG, "nvs_commit attrs: %s", esp_err_to_name(err));
    return err == ESP_OK;
}

static bool nvs_save_config(uint64_t ieee, const DeviceConfig* cfg) {
    if (!s_nvs) return false;
    char key[16];
    shadow_key(key, 'c', ieee);   // Q76: full 64-bit IEEE, base36
    esp_err_t err = nvs_set_blob(s_nvs, key, cfg, sizeof(DeviceConfig));
    if (err != ESP_OK) { ESP_LOGE(TAG, "nvs_set_blob config: %s", esp_err_to_name(err)); return false; }
    err = nvs_commit(s_nvs);
    if (err != ESP_OK) ESP_LOGE(TAG, "nvs_commit config: %s", esp_err_to_name(err));
    return err == ESP_OK;
}

// F26 (FINDINGS.md): config setters previously hit NVS on every API call.
// Dedupe by CRC of the config bytes — a no-op re-push (e.g. SPA re-sending an
// unchanged config) skips the flash write entirely; a real change still
// persists before the setter returns, so no edit is ever lost. Split into a
// decide-under-lock half and a write-outside-lock half so the flash write
// never runs under s_mutex (leaf-lock rule).
struct CfgPersist {
    bool         need;
    uint64_t     ieee;
    uint32_t     crc;
    DeviceConfig cfg;    // snapshot — the entry may mutate again after unlock
};

// Caller holds s_mutex. Decides whether the (already-updated) entry config
// needs a flash write and snapshots it if so.
static void persist_config_prepare_locked(const DeviceShadowEntry* e, CfgPersist* out) {
    out->need = false;
    uint32_t crc = esp_rom_crc32_le(0, reinterpret_cast<const uint8_t*>(&e->config),
                                    sizeof(DeviceConfig));
    if (e->cfg_crc_valid && crc == e->cfg_crc) return;   // unchanged → no flash wear
    out->need = true;
    out->ieee = e->ieee;
    out->crc  = crc;
    out->cfg  = e->config;
}

// Caller must NOT hold s_mutex. Writes the snapshot, then re-locks briefly to
// commit the dedupe CRC on success (on failure the CRC stays stale, so the
// next setter call retries the write). Two concurrent setters race benignly:
// each NVS blob is internally consistent (own snapshot), last write wins.
static void persist_config_commit(const CfgPersist* p) {
    if (!p->need) return;
    if (!nvs_save_config(p->ieee, &p->cfg)) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    DeviceShadowEntry* e = find_entry(p->ieee);
    if (e) {
        e->cfg_crc       = p->crc;
        e->cfg_crc_valid = true;
    }
    xSemaphoreGive(s_mutex);
}

static bool nvs_load_attrs(uint64_t ieee, ShadowAttr* out, uint8_t* count) {
    if (!s_nvs) return false;
    char key[16];
    shadow_key(key, 'a', ieee);   // Q76: full 64-bit IEEE, base36

    // F26: read into scratch, then validate header (version), exact length,
    // and CRC before trusting the payload. Any mismatch → discard (entry
    // starts empty and re-populates from live traffic) rather than loading a
    // truncated / corrupt final attr.
    size_t len = sizeof(s_attr_blob);
    esp_err_t e = nvs_get_blob(s_nvs, key, s_attr_blob, &len);
    if (e != ESP_OK) return false;
    if (len < sizeof(ShadowBlobHdr)) {
        ESP_LOGW(TAG, "attr blob too short (%zu B) — discarding", len);
        return false;
    }
    const auto* h = reinterpret_cast<const ShadowBlobHdr*>(s_attr_blob);
    if (h->ver != SHADOW_ATTR_BLOB_VER || h->count > SHADOW_ATTR_MAX) {
        ESP_LOGW(TAG, "attr blob ver/count invalid (ver=%u count=%u) — discarding",
                 h->ver, h->count);
        return false;
    }
    const size_t plen = (size_t)h->count * sizeof(ShadowAttr);
    if (len != sizeof(ShadowBlobHdr) + plen) {
        ESP_LOGW(TAG, "attr blob size mismatch (%zu != %zu) — discarding",
                 len, sizeof(ShadowBlobHdr) + plen);
        return false;
    }
    const uint8_t* pay = s_attr_blob + sizeof(ShadowBlobHdr);
    if (esp_rom_crc32_le(0, pay, plen) != h->crc) {
        ESP_LOGW(TAG, "attr blob CRC mismatch — discarding");
        return false;
    }
    memcpy(out, pay, plen);
    *count = h->count;
    return true;
}

static bool nvs_load_config(uint64_t ieee, DeviceConfig* out) {
    if (!s_nvs) return false;
    char key[16];
    shadow_key(key, 'c', ieee);   // Q76: full 64-bit IEEE, base36
    size_t len = sizeof(DeviceConfig);
    return (nvs_get_blob(s_nvs, key, out, &len) == ESP_OK);
}

// The ONLY persistence hook on the hot paths. Pre-fix, eligible entries were
// written to flash inline (under s_mutex, on the radio RX path — :412); now
// every attr write is deferred to the task_shadow sweep, which honours
// NVS_MIN_INTERVAL_S per device unless `force` and runs the flash I/O with
// the lock released. Worst-case added persistence latency: one sweep period
// (SWEEP_PERIOD_MS). Caller holds s_mutex.
static void mark_attrs_dirty(DeviceShadowEntry* e, bool force = false) {
    e->nvs_dirty = true;
    if (force) e->nvs_force = true;
}

// ── Shadow table lookup ───────────────────────────────────────────────────

static DeviceShadowEntry* find_entry(uint64_t ieee) {
    for (uint16_t i = 0; i < s_count; i++) {
        if (s_shadow[i].ieee == ieee) return &s_shadow[i];
    }
    return nullptr;
}

static DeviceShadowEntry* find_or_create_entry(uint64_t ieee) {
    DeviceShadowEntry* e = find_entry(ieee);
    if (e) return e;
    if (s_count >= ZAP_MAX_DEVICES) return nullptr;
    e = &s_shadow[s_count++];
    memset(e, 0, sizeof(DeviceShadowEntry));
    e->ieee = ieee;
    e->config.last_seen_enabled = true;
    return e;
}

// ── Forward declarations ─────────────────────────────────────────────────
static void upsert_cache(DeviceShadowEntry* e, const ZclAttribute* attrs, uint8_t count);

// ── Staged ZCL_ATTR events ────────────────────────────────────────────────
// Pre-fix, emit_zcl_attr() published to the event bus while holding s_mutex
// (:334). Surviving attrs are now staged into s_staged under the lock and
// published only after xSemaphoreGive — see the lock-discipline block above.
// One shared buffer (s_emit_mutex owns it) instead of per-call stack arrays:
// at 84 B per ZclAttribute a 32-slot array is ~2.7 KB, too rich for tasks
// whose stacks were sized after the apply_pipeline statics were carved out
// (the old 8 KB zcl_attr overflow), so it lives in PSRAM like the table.
// Staging is per-device: publish before staging a different entry.
struct StagedAttrs {
    uint64_t     ieee;
    uint8_t      count;
    ZclAttribute attrs[SHADOW_ATTR_MAX];
};
EXT_RAM_BSS_ATTR static StagedAttrs s_staged;

// Truncation accounting for stage_attrs(): attrs past SHADOW_ATTR_MAX in one
// staging window are dropped from the ZCL_ATTR event stream only (the cache
// upsert has already run, so RAM/NVS state is unaffected). Counter is
// volatile for debugger/console reads; it is only mutated under s_emit_mutex.
static volatile uint32_t s_staged_drop_count  = 0;
static bool              s_staged_drop_logged = false;

// Caller holds s_emit_mutex (asserted) + s_mutex.
static void stage_attrs(const DeviceShadowEntry* e, const ZclAttribute* attrs, uint8_t count) {
    configASSERT(xSemaphoreGetMutexHolder(s_emit_mutex) == xTaskGetCurrentTaskHandle());
    // Staging is per-device (see the StagedAttrs block comment): callers must
    // publish_staged() before staging a different entry, or the earlier
    // entry's attrs would be mis-attributed to this ieee.
    configASSERT(s_staged.count == 0 || s_staged.ieee == e->ieee);
    s_staged.ieee = e->ieee;
    for (uint8_t i = 0; i < count; i++) {
        if (s_staged.count >= SHADOW_ATTR_MAX) {
            // No compound-assign on volatile (deprecated since C++20).
            s_staged_drop_count = s_staged_drop_count + (uint32_t)(count - i);
            if (!s_staged_drop_logged) {
                s_staged_drop_logged = true;
                ESP_LOGW(TAG, "staged attr buffer full (%u) — dropping %u ZCL_ATTR "
                              "event(s); cache state unaffected (total dropped %u)",
                         (unsigned)SHADOW_ATTR_MAX, (unsigned)(count - i),
                         (unsigned)s_staged_drop_count);
            }
            break;
        }
        s_staged.attrs[s_staged.count++] = attrs[i];
    }
}

// Caller holds s_emit_mutex (asserted) and must have RELEASED s_mutex.
// Publishes the staged attrs and resets the buffer.
static void publish_staged() {
    configASSERT(xSemaphoreGetMutexHolder(s_emit_mutex) == xTaskGetCurrentTaskHandle());
    for (uint8_t i = 0; i < s_staged.count; i++) {
        const ZclAttribute& a = s_staged.attrs[i];
        Event ev{};
        ev.type = EventType::ZCL_ATTR;
        ZclAttrEvent* payload = reinterpret_cast<ZclAttrEvent*>(ev.data);
        payload->ieee     = s_staged.ieee;
        payload->val_type = a.val_type;
        payload->cluster  = a.cluster;
        payload->attr_id  = a.attr_id;
        // memcpy + explicit NUL avoids the -Wstringop-truncation noise
        // -O2 emits when strncpy's dest size equals the source's worst-
        // case length (the next-line NUL keeps it correct, but gcc's
        // analysis can't prove that).
        memcpy(payload->key, a.key, ATTR_KEY_MAX - 1);
        payload->key[ATTR_KEY_MAX - 1] = '\0';
        if (a.val_type == VAL_STR) {
            memcpy(payload->str_val, a.str_val, ATTR_STR_MAX);
        } else {
            payload->int_val = a.int_val;
        }
        event_bus_publish(ev);
    }
    s_staged.count = 0;
}

// ── Debounce timer callback ──────────────────────────────────────────────

// Log-once for the queue-full fallback below (parity with the occupancy
// callback). File-scope volatile: written on the timer service task, read
// anywhere; it only gates log spam, so racy exactness is fine.
static volatile bool s_debounce_q_full_logged = false;

static void debounce_timer_cb(TimerHandle_t xTimer) {
    DeviceShadowEntry* e = static_cast<DeviceShadowEntry*>(pvTimerGetTimerID(xTimer));
    ShadowTaskMsg msg{ e->ieee, ShadowMsgKind::DebounceFlush };
    if (xQueueSend(s_task_queue, &msg, 0) != pdTRUE) {
        e->debounce_pending_flush = true;
        if (!s_debounce_q_full_logged) {
            s_debounce_q_full_logged = true;
            ESP_LOGW(TAG, "shadow task queue full — debounce flush deferred to sweep");
        }
    }
}

// ── Occupancy timeout callback ───────────────────────────────────────────

// Runs on the SHARED FreeRTOS timer service task. Pre-fix (:248) it blocked
// here on s_mutex with portMAX_DELAY — while an NVS sweep held the lock,
// every software timer in the firmware froze. Now lock-free: zero-timeout
// enqueue; task_shadow does the locked work. On a full queue, fall back to a
// per-entry flag the SWEEP_PERIOD_MS sweep picks up — nothing is dropped, the timeout
// just lands one sweep late. Entry slots are never freed (the table is
// append-only), so the unlocked flag store cannot dangle; same pattern as
// debounce_pending_flush.
static void occupancy_timeout_cb(TimerHandle_t timer) {
    DeviceShadowEntry* e = static_cast<DeviceShadowEntry*>(pvTimerGetTimerID(timer));
    ShadowTaskMsg msg{ e->ieee, ShadowMsgKind::OccupancyTimeout };
    if (xQueueSend(s_task_queue, &msg, 0) != pdTRUE) {
        e->occupancy_timeout_pending = true;
        static bool s_q_full_logged = false;
        if (!s_q_full_logged) {
            s_q_full_logged = true;
            ESP_LOGW(TAG, "shadow task queue full — occupancy timeout deferred to sweep");
        }
    }
}

// Synthetic occupancy=0 once the per-device TTL expires. Runs on task_shadow.
// Caller holds s_emit_mutex + s_mutex and publishes after release. Q17
// (QWEN_FINDINGS triage): persistence stays deferred — mark dirty only, the
// sweep writes flash outside the lock.
static void occupancy_apply_locked(DeviceShadowEntry* e) {
    e->occupancy_timeout_pending = false;
    if (e->config.occupancy_timeout_s == 0) return;
    // A fresh occupancy=1 report may have re-armed the TTL timer between the
    // timeout enqueue and this drain (the queue hop adds up to one sweep
    // period) — synthesizing occupancy=0 now would clobber live state. An
    // armed timer means the timeout that queued this message is obsolete.
    // (The old run-in-callback code had the same race, just a narrower
    // window; this closes it.)
    if (e->occupancy_timer && xTimerIsTimerActive(e->occupancy_timer)) return;

    ZclAttribute synthetic{};
    zcl_attr_set_int(&synthetic, KEY_OCCUPANCY, 0, VAL_BOOL);
    synthetic.cluster = 0x0406;
    synthetic.attr_id = 0x0000;

    upsert_cache(e, &synthetic, 1);
    stage_attrs(e, &synthetic, 1);
    mark_attrs_dirty(e);

    ESP_LOGD(TAG, "occ_ttl: 0x%014llX — synthetic occupancy=0",
             (unsigned long long)(e->ieee & 0x00FFFFFFFFFFFFFFULL));
}

// ── Cache upsert ──────────────────────────────────────────────────────────

static void upsert_cache(DeviceShadowEntry* e, const ZclAttribute* attrs, uint8_t count) {
    uint32_t now_s = (uint32_t)(xTaskGetTickCount() / configTICK_RATE_HZ);
    for (uint8_t i = 0; i < count; i++) {
        bool found = false;
        for (uint8_t j = 0; j < e->attr_count; j++) {
            if (strncmp(e->attrs[j].key, attrs[i].key, ATTR_KEY_MAX) == 0) {
                e->attrs[j].val_type = attrs[i].val_type;
                if (attrs[i].val_type == VAL_STR) {
                    memcpy(e->attrs[j].str_val, attrs[i].str_val, ATTR_STR_MAX);
                } else {
                    e->attrs[j].int_val = attrs[i].int_val;
                }
                e->attrs[j].ts = now_s;
                found = true;
                break;
            }
        }
        if (!found && e->attr_count < 32) {
            ShadowAttr& sa = e->attrs[e->attr_count++];
            memset(&sa, 0, sizeof(sa));
            strncpy(sa.key, attrs[i].key, ATTR_KEY_MAX - 1);
            sa.key[ATTR_KEY_MAX - 1] = '\0';
            sa.val_type = attrs[i].val_type;
            if (attrs[i].val_type == VAL_STR) {
                memcpy(sa.str_val, attrs[i].str_val, ATTR_STR_MAX);
            } else {
                sa.int_val = attrs[i].int_val;
            }
            sa.ts = now_s;
        }
    }
}

// ── Apply pipeline and stage surviving attrs ──────────────────────────────

static void apply_pipeline_and_stage(DeviceShadowEntry* e,
                                      const ZclAttribute* attrs, uint8_t count)
{
    // `ZclAttribute` grew in the 2026-04-20 attr_keys drop (84 B today).
    // Three 32-slot local arrays would chew ~8 KB of the zcl_attr task's
    // 8 KB stack — overflow observed on Xiaomi cube interview.
    // Caller holds `s_mutex` for the entire duration of this function so
    // `static` buffers are safe (single-threaded access guaranteed); they
    // are fully consumed (staged/upserted) before the lock is released.
    // PSRAM (EXT_RAM_BSS_ATTR): ~10.7 KB across the four 32-slot arrays —
    // warm path (per-report staging, not ISR/DMA), internal DRAM is tighter.
    EXT_RAM_BSS_ATTR static ZclAttribute filtered[32];
    uint8_t n = shadow_pipeline_filter(&e->config, attrs, count, filtered, 32);
    if (n == 0) return;

    if (e->config.occupancy_timeout_s > 0) {
        for (uint8_t i = 0; i < n; i++) {
            if (strncmp(filtered[i].key, KEY_OCCUPANCY, ATTR_KEY_MAX) == 0
                && filtered[i].int_val == 1) {
                if (!e->occupancy_timer) {
                    e->occupancy_timer = xTimerCreate("occ_ttl",
                        pdMS_TO_TICKS((uint32_t)e->config.occupancy_timeout_s * 1000UL),
                        pdFALSE, static_cast<void*>(e), occupancy_timeout_cb);
                }
                if (e->occupancy_timer) {
                    xTimerChangePeriod(e->occupancy_timer,
                        pdMS_TO_TICKS((uint32_t)e->config.occupancy_timeout_s * 1000UL), 0);
                    xTimerReset(e->occupancy_timer, 0);
                }
                break;
            }
        }
    }

    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

    if (e->config.debounce_ms > 0) {
        EXT_RAM_BSS_ATTR static ZclAttribute bypass[32];
        uint8_t bypass_count = 0;
        EXT_RAM_BSS_ATTR static ZclAttribute merge[32];
        uint8_t merge_count = 0;

        for (uint8_t i = 0; i < n; i++) {
            int8_t bi = shadow_pipeline_debounce_bypass(&e->config, &e->pending, &filtered[i]);
            if (bi >= 0) {
                bypass[bypass_count++] = filtered[i];
            } else {
                merge[merge_count++] = filtered[i];
            }
        }

        if (merge_count > 0)
            shadow_pipeline_merge_pending(&e->config, &e->pending, merge, merge_count);

        if (!e->debounce_timer) {
            e->debounce_timer = xTimerCreate("dbounce",
                pdMS_TO_TICKS(e->config.debounce_ms), pdFALSE,
                static_cast<void*>(e), debounce_timer_cb);
        }
        if (e->debounce_timer) {
            xTimerReset(e->debounce_timer, 0);
        } else {
            // Creation failed (timer heap exhausted) — pre-fix this fell
            // through to xTimerReset(NULL) and tripped configASSERT (:396).
            // Guard like the occupancy timer above, but also flag the entry:
            // without a timer the merged attrs would sit in `pending`
            // forever, so let the SWEEP_PERIOD_MS sweep flush them (debounce
            // degrades to ~sweep latency, nothing is dropped).
            e->debounce_pending_flush = true;
        }

        if (bypass_count > 0) {
            upsert_cache(e, bypass, bypass_count);
            stage_attrs(e, bypass, bypass_count);
        }

    } else if (e->config.throttle_ms > 0) {
        if (!shadow_pipeline_throttle_pass(&e->config, &e->throttle_last_ms, now_ms))
            return;
        upsert_cache(e, filtered, n);
        stage_attrs(e, filtered, n);
        mark_attrs_dirty(e);
    } else {
        upsert_cache(e, filtered, n);
        stage_attrs(e, filtered, n);
        mark_attrs_dirty(e);
    }
}

static void flush_pending_entry_locked(DeviceShadowEntry* e) {
    // Static — caller holds s_mutex, same reason as apply_pipeline_and_stage.
    // PSRAM for the same reason as the pipeline arrays above.
    EXT_RAM_BSS_ATTR static ZclAttribute out[32];
    uint8_t n = shadow_pipeline_flush_pending(&e->pending, out, 32);
    if (n > 0) {
        upsert_cache(e, out, n);
        stage_attrs(e, out, n);
        mark_attrs_dirty(e);
    }
    e->debounce_pending_flush = false;
}

// ── task_shadow: debounce/occupancy housekeeping + deferred persistence ───

// task_shadow-exclusive flash scratch (PSRAM). Only this task serializes
// attr blobs for WRITING (boot restore loads via the separate s_attr_blob),
// so no lock guards it — and its ~2.8 KB would not fit the task's 4 KB stack
// (task_stacks.h kDeviceShadow).
EXT_RAM_BSS_ATTR static uint8_t s_sweep_blob[sizeof(ShadowBlobHdr) + SHADOW_ATTR_MAX * sizeof(ShadowAttr)];

// Pre-fix (:446) the sweep held s_mutex across the FULL table doing
// sequential nvs_set_blob+commit per dirty entry — stalling the radio RX
// path, all readers and (via the old occupancy callback, :248) the FreeRTOS
// timer service task for the whole sweep. Now the locks are cycled per
// entry: lock → flush/serialize into task-owned scratch + clear flags →
// unlock → publish + flash write OUTSIDE the locks. Worst-case s_mutex hold
// is one entry's RAM work; flash latency never blocks the lock. The table is
// append-only (entries are never removed or compacted), so indices and
// pointers stay valid across the unlock windows; s_count is re-read under
// the lock each round.
static void task_shadow(void* arg) {
    ShadowTaskMsg msg{};
    for (;;) {
        bool got_msg = (xQueueReceive(s_task_queue, &msg, pdMS_TO_TICKS(SWEEP_PERIOD_MS)) == pdTRUE);

        if (got_msg) {
            xSemaphoreTake(s_emit_mutex, portMAX_DELAY);
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            DeviceShadowEntry* e = find_entry(msg.ieee);
            if (e) {
                if (msg.kind == ShadowMsgKind::DebounceFlush) {
                    flush_pending_entry_locked(e);
                } else {
                    occupancy_apply_locked(e);
                }
            }
            xSemaphoreGive(s_mutex);
            publish_staged();
            xSemaphoreGive(s_emit_mutex);
        }

        for (uint16_t i = 0; ; i++) {
            size_t   blob_len = 0;
            uint64_t ieee     = 0;

            xSemaphoreTake(s_emit_mutex, portMAX_DELAY);
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            if (i >= s_count) {
                xSemaphoreGive(s_mutex);
                xSemaphoreGive(s_emit_mutex);
                break;
            }
            DeviceShadowEntry* e = &s_shadow[i];
            if (e->debounce_pending_flush)    flush_pending_entry_locked(e);
            if (e->occupancy_timeout_pending) occupancy_apply_locked(e);

            uint32_t now_s = (uint32_t)(xTaskGetTickCount() / configTICK_RATE_HZ);
            if (e->nvs_dirty &&
                (e->nvs_force || (now_s - e->nvs_last_write_s) >= NVS_MIN_INTERVAL_S)) {
                ieee     = e->ieee;
                blob_len = attr_blob_serialize(s_sweep_blob, e->attrs, e->attr_count);
                // Stamp + clear optimistically under the lock. An update
                // landing during the unlocked flash write below re-marks
                // dirty and persists on its next eligible sweep; a FAILED
                // write re-marks dirty after the stamp, giving a natural
                // NVS_MIN_INTERVAL_S backoff instead of hammering a failing
                // flash (and spamming ESP_LOGE) every SWEEP_PERIOD_MS.
                e->nvs_dirty        = false;
                e->nvs_force        = false;
                e->nvs_last_write_s = now_s;
            }
            xSemaphoreGive(s_mutex);
            publish_staged();
            xSemaphoreGive(s_emit_mutex);

            if (blob_len > 0 && !nvs_write_attr_blob(ieee, s_sweep_blob, blob_len)) {
                xSemaphoreTake(s_mutex, portMAX_DELAY);
                e->nvs_dirty = true;   // entry slots are stable — e still valid
                xSemaphoreGive(s_mutex);
            }
        }
    }
}

// ── Public API ────────────────────────────────────────────────────────────

void device_shadow_init() {
    // NVS format version guard. v6 = ATTR_KEY_MAX widened 20→28 +
    // ATTR_STR_MAX widened 32→48 so ShadowAttr grew 60 → 84 bytes
    // (SHA-F1 + SHA-F8 in docs/FINDINGS.md). v7 = F26: attr blob gained a
    // version+count+CRC header (ShadowBlobHdr), so the raw v6 layout no longer
    // validates — wipe it on first boot rather than rejecting per-device on load.
    static constexpr uint8_t NVS_SHADOW_VERSION = 7;
    if (nvs_open(NVS_NS, NVS_READWRITE, &s_nvs) == ESP_OK) {
        uint8_t ver = 0;
        if (nvs_get_u8(s_nvs, "ver", &ver) != ESP_OK || ver != NVS_SHADOW_VERSION) {
            esp_err_t acc = ESP_OK;
            nvs_seq(&acc, nvs_erase_all(s_nvs), TAG, "erase_all shadow");
            if (acc == ESP_OK) {
                // Version marker only after a clean wipe — writing v7 over
                // a failed erase would park stale v6 blobs behind the new
                // marker (the F26 header check rejects them on load, but
                // the namespace must not claim an upgrade it didn't do).
                nvs_seq(&acc, nvs_set_u8(s_nvs, "ver", NVS_SHADOW_VERSION),
                        TAG, "set_u8 ver");
                nvs_seq(&acc, nvs_commit(s_nvs), TAG, "commit ver");
            }
            if (acc == ESP_OK)
                ESP_LOGW(TAG, "shadow NVS format upgraded v%d — cache cleared", NVS_SHADOW_VERSION);
            else
                ESP_LOGE(TAG, "shadow NVS upgrade incomplete — will retry next boot");
        }
    } else {
        // SHA-F12: surface the consequence — without persistence the
        // cache works in RAM only and resets every reboot. Existing
        // save_* paths gate on `s_nvs == 0` so operations stay safe.
        ESP_LOGE(TAG, "nvs_open shadow namespace failed — cache will run "
                       "without persistence (state lost on reboot)");
        s_nvs = 0;
    }

    s_mutex = xSemaphoreCreateMutex();
    configASSERT(s_mutex);
    s_emit_mutex = xSemaphoreCreateMutex();
    configASSERT(s_emit_mutex);
    s_task_queue = xQueueCreate(TASK_QUEUE_DEPTH, sizeof(ShadowTaskMsg));
    configASSERT(s_task_queue);

    s_shadow = static_cast<DeviceShadowEntry*>(
        heap_caps_calloc(ZAP_MAX_DEVICES, sizeof(DeviceShadowEntry), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    configASSERT(s_shadow);

    xTaskCreatePinnedToCore(task_shadow, "task_shadow", zhac::stack::kDeviceShadow, nullptr, 4, nullptr, 1);
    ESP_LOGI(TAG, "device_shadow init OK");
}

uint16_t device_shadow_restore_from_pool(const ZapDevice* pool, uint16_t count) {
    // Pre-fix (:500) this mutated s_shadow/s_count and loaded through the
    // shared s_attr_blob scratch with NO lock, racing task_shadow (spawned
    // earlier in init) which iterates the table every SWEEP_PERIOD_MS. Now each
    // device's NVS reads happen OUTSIDE s_mutex into a boot-only scratch
    // (leaf-lock rule), the table install goes under the lock, and the
    // DEVICE_JOIN publish + zap_store_mark_dirty run after release.
    // Spawning task_shadow after restore instead was rejected: restore is
    // invoked by firmware after init() returns, so reordering would need a
    // split init/start API rippling through both firmware cores.
    // Boot-only scratch (PSRAM): restore is documented single-call/
    // single-task; ~3.2 KB is too rich for the caller's stack.
    static EXT_RAM_BSS_ATTR struct {
        DeviceConfig cfg;
        ShadowAttr   attrs[SHADOW_ATTR_MAX];
    } sc;

    uint16_t restored = 0;
    for (uint16_t i = 0; i < count; i++) {
        const ZapDevice* d = &pool[i];

        bool has_cfg = nvs_load_config(d->ieee_addr, &sc.cfg);
        uint32_t cfg_crc = 0;
        if (has_cfg) {
            // F26: seed the dedupe CRC so an unchanged config re-push after
            // boot doesn't trigger a redundant NVS write.
            cfg_crc = esp_rom_crc32_le(0, reinterpret_cast<const uint8_t*>(&sc.cfg),
                                       sizeof(DeviceConfig));
        }
        uint8_t ac = 0;
        bool has_attrs = nvs_load_attrs(d->ieee_addr, sc.attrs, &ac);

        ZapDevice patched{};
        bool patch_last_seen = false;

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        DeviceShadowEntry* e = find_or_create_entry(d->ieee_addr);
        if (!e) {
            xSemaphoreGive(s_mutex);
            continue;
        }
        if (has_cfg) {
            e->config        = sc.cfg;
            e->cfg_crc       = cfg_crc;
            e->cfg_crc_valid = true;
        }
        if (has_attrs && ac > 0) {
            memcpy(e->attrs, sc.attrs, (size_t)ac * sizeof(ShadowAttr));
            e->attr_count = ac;
        }
        // Restore last_seen from synthetic attr → ZapDevice. Deferred via
        // mark_dirty(LOW) so the flush task batches the write outside the
        // boot critical path.
        for (uint8_t j = 0; j < e->attr_count; j++) {
            if (strncmp(e->attrs[j].key, KEY_LAST_SEEN, ATTR_KEY_MAX) == 0) {
                patched = *d;
                patched.last_seen = (uint32_t)e->attrs[j].int_val;
                patch_last_seen = true;
                break;
            }
        }
        xSemaphoreGive(s_mutex);

        if (patch_last_seen) zap_store_mark_dirty(&patched, ZAP_PERSIST_LOW);

        Event ev{};
        ev.type = EventType::DEVICE_JOIN;
        uint64_t ieee = d->ieee_addr;
        memcpy(ev.data, &ieee, sizeof(ieee));
        event_bus_publish(ev);
        restored++;
    }
    ESP_LOGI(TAG, "boot restore: %u devices", restored);
    return restored;
}

void device_shadow_process(const ZapDevice* dev,
                           const ZclAttribute* attrs, uint8_t count)
{
    _METRIC_TIMER_SCOPE(METRIC_SHADOW_PROCESS);
    // Radio RX path. Pre-fix (:412) this committed attr blobs to NVS while
    // holding s_mutex; persistence is now a dirty-mark handled by the
    // task_shadow sweep, and the bus publish happens after the lock drops.
    xSemaphoreTake(s_emit_mutex, portMAX_DELAY);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    DeviceShadowEntry* e = find_or_create_entry(dev->ieee_addr);
    if (!e) {
        xSemaphoreGive(s_mutex);
        xSemaphoreGive(s_emit_mutex);
        return;
    }

    apply_pipeline_and_stage(e, attrs, count);

    // Inject synthetic _last_seen (bypasses pipeline — internal bookkeeping)
    if (e->config.last_seen_enabled) {
        ZclAttribute ls{};
        zcl_attr_set_int(&ls, KEY_LAST_SEEN, (int32_t)time(nullptr), VAL_INT);
        ls.cluster = 0xFFFF;
        ls.attr_id = 0xFFFF;
        upsert_cache(e, &ls, 1);
    }

    xSemaphoreGive(s_mutex);
    publish_staged();
    xSemaphoreGive(s_emit_mutex);
}

void device_shadow_update_optimistic(uint64_t ieee, const char* key,
                                     uint8_t val_type, int32_t int_val)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    DeviceShadowEntry* e = find_entry(ieee);
    if (e && key) {
        ZclAttribute a{};
        zcl_attr_set_int(&a, key, int_val, static_cast<ValType>(val_type));
        upsert_cache(e, &a, 1);
    }
    xSemaphoreGive(s_mutex);
}

uint8_t device_shadow_get_attrs(uint64_t ieee, ShadowAttr* out, uint8_t max_count) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    DeviceShadowEntry* e = find_entry(ieee);
    if (!e) { xSemaphoreGive(s_mutex); return 0; }
    uint8_t n = (e->attr_count < max_count) ? e->attr_count : max_count;
    for (uint8_t i = 0; i < n; i++) out[i] = e->attrs[i];
    xSemaphoreGive(s_mutex);
    return n;
}

// P2-T18 def 7 (FINDINGS §7): single-attr read by key. LEAF lock only (T10):
// take s_mutex, find_entry, linear key scan, copy out, release. No NVS, no
// publish, no nested lock — so it is safe on the event-drain task and lets
// the ZIGBEE_TOGGLE action avoid a ~2.7 KB ShadowAttr[32] + 522 B device
// snapshot on that shared stack.
bool device_shadow_get_attr(uint64_t ieee, const char* key, ShadowAttr* out) {
    if (!key || !out) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    DeviceShadowEntry* e = find_entry(ieee);
    bool found = false;
    if (e) {
        for (uint8_t i = 0; i < e->attr_count; i++) {
            if (strncmp(e->attrs[i].key, key, ATTR_KEY_MAX) == 0) {
                *out = e->attrs[i];
                found = true;
                break;
            }
        }
    }
    xSemaphoreGive(s_mutex);
    return found;
}

bool device_shadow_set_config(uint64_t ieee, const DeviceConfig* cfg) {
    CfgPersist cp;
    xSemaphoreTake(s_emit_mutex, portMAX_DELAY);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    DeviceShadowEntry* e = find_or_create_entry(ieee);
    if (!e) {
        xSemaphoreGive(s_mutex);
        xSemaphoreGive(s_emit_mutex);
        return false;
    }

    flush_pending_entry_locked(e);
    // Pre-fix this force-persisted flushed attrs synchronously (under the
    // lock). Same intent, leaf-lock safe: the force flag makes the next
    // sweep (≤ SWEEP_PERIOD_MS) write regardless of NVS_MIN_INTERVAL_S.
    if (e->nvs_dirty) e->nvs_force = true;

    e->config = *cfg;
    persist_config_prepare_locked(e, &cp);
    xSemaphoreGive(s_mutex);
    publish_staged();
    xSemaphoreGive(s_emit_mutex);

    persist_config_commit(&cp);
    return true;
}

bool device_shadow_set_throttle_ms(uint64_t ieee, uint32_t throttle_ms) {
    CfgPersist cp;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    DeviceShadowEntry* e = find_or_create_entry(ieee);
    if (!e) { xSemaphoreGive(s_mutex); return false; }
    e->config.throttle_ms = throttle_ms;
    e->throttle_last_ms = 0;   // reset window so the new rate applies immediately
    persist_config_prepare_locked(e, &cp);
    xSemaphoreGive(s_mutex);
    persist_config_commit(&cp);
    return true;
}

bool device_shadow_get_config(uint64_t ieee, DeviceConfig* out) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    DeviceShadowEntry* e = find_entry(ieee);
    if (!e) { xSemaphoreGive(s_mutex); return false; }
    *out = e->config;
    xSemaphoreGive(s_mutex);
    return true;
}

void device_shadow_clear_attrs(uint64_t ieee) {
    bool found = false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    DeviceShadowEntry* e = find_entry(ieee);
    if (e) {
        found = true;
        memset(e->attrs, 0, sizeof(e->attrs));
        e->attr_count = 0;

        // 0-tick timer posts — non-blocking, allowed under the leaf lock.
        if (e->occupancy_timer) { xTimerDelete(e->occupancy_timer, 0); e->occupancy_timer = nullptr; }
        if (e->debounce_timer)  { xTimerDelete(e->debounce_timer, 0);  e->debounce_timer  = nullptr; }

        memset(&e->pending, 0, sizeof(e->pending));
        e->throttle_last_ms = 0;
        e->debounce_pending_flush = false;
        e->occupancy_timeout_pending = false;
        e->nvs_dirty = false;
        e->nvs_force = false;
    }
    xSemaphoreGive(s_mutex);

    // Leaf-lock rule: the NVS erase moved outside s_mutex. Worst-case
    // interleave: the sweep serialized this entry just before the clear and
    // its unlocked flash write lands AFTER the erase, resurrecting the
    // pre-clear blob until the device's next persisted update (rejoin
    // traffic re-marks dirty, so ≤ NVS_MIN_INTERVAL_S). Millisecond-wide
    // window, RAM state stays correct, and only a reboot inside it would
    // ever load the stale blob — accepted over flash I/O under the lock.
    bool nvs_ok = true;
    if (found && s_nvs) {
        char key[16];
        shadow_key(key, 'a', ieee);   // Q76: full 64-bit IEEE, base36
        esp_err_t e = nvs_erase_key(s_nvs, key);
        if (e != ESP_ERR_NVS_NOT_FOUND) {  // never persisted → nothing to do
            // nullptr acc: nvs_seq still logs each failing op; the two
            // results gate the summary line below. `&=` keeps the commit
            // attempted even after a failed erase (no short-circuit).
            nvs_ok = nvs_seq(nullptr, e, TAG, "erase_key attr clear") == ESP_OK;
            nvs_ok &= nvs_seq(nullptr, nvs_commit(s_nvs), TAG,
                              "commit attr clear") == ESP_OK;
        }
    }
    if (nvs_ok) {
        ESP_LOGI(TAG, "Cleared shadow attr cache ieee=0x%016llX",
                 (unsigned long long)ieee);
    } else {
        ESP_LOGW(TAG, "Shadow attr cache ieee=0x%016llX: RAM cleared; NVS erase "
                      "failed — cached attrs may resurface on reboot",
                 (unsigned long long)ieee);
    }
}

bool device_shadow_set_debounce_ms(uint64_t ieee, uint32_t debounce_ms) {
    CfgPersist cp;
    xSemaphoreTake(s_emit_mutex, portMAX_DELAY);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    DeviceShadowEntry* e = find_or_create_entry(ieee);
    if (!e) {
        xSemaphoreGive(s_mutex);
        xSemaphoreGive(s_emit_mutex);
        return false;
    }

    // Flushing any queued pending state under the OLD window keeps
    // buffered updates from being silently dropped when the user
    // reduces or disables debounce.
    flush_pending_entry_locked(e);
    e->config.debounce_ms = debounce_ms;

    // If debounce was disabled, drop the idle timer so no ghost flush.
    if (debounce_ms == 0 && e->debounce_timer) {
        xTimerDelete(e->debounce_timer, 0);
        e->debounce_timer = nullptr;
    }

    persist_config_prepare_locked(e, &cp);
    xSemaphoreGive(s_mutex);
    publish_staged();
    xSemaphoreGive(s_emit_mutex);

    persist_config_commit(&cp);
    return true;
}

bool device_shadow_set_occupancy_timeout(uint64_t ieee, uint16_t timeout_s) {
    CfgPersist cp;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    DeviceShadowEntry* e = find_entry(ieee);
    if (!e) { xSemaphoreGive(s_mutex); return false; }

    e->config.occupancy_timeout_s = timeout_s;

    if (timeout_s == 0 && e->occupancy_timer) {
        xTimerDelete(e->occupancy_timer, 0);
        e->occupancy_timer = nullptr;
    }

    persist_config_prepare_locked(e, &cp);
    xSemaphoreGive(s_mutex);
    persist_config_commit(&cp);
    return true;
}
