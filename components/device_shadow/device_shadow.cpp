// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "device_shadow.h"
#include "event_bus.h"
#include "zap_store.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "metrics/metrics_macros.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_rom_crc.h"   // F26: esp_rom_crc32_le for blob integrity
#include <cstring>
#include <cstdio>
#include <ctime>
#include "task_stacks.h"

static const char* TAG = "device_shadow";
static const char* NVS_NS = "zap_shadow";
static constexpr uint32_t NVS_MIN_INTERVAL_S = 300; // max one NVS write per 5min per device

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
// Single shared scratch: save() runs under s_mutex (serialised across tasks),
// load() runs only at boot (single-threaded restore) — never concurrent.
static uint8_t s_attr_blob[sizeof(ShadowBlobHdr) + SHADOW_ATTR_MAX * sizeof(ShadowAttr)];

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
static SemaphoreHandle_t s_mutex;

struct DebounceFireMsg { uint64_t ieee; };
static QueueHandle_t s_debounce_queue;

// ── NVS helpers ───────────────────────────────────────────────────────────

static bool nvs_save_attrs(uint64_t ieee, const ShadowAttr* attrs, uint8_t count) {
    if (!s_nvs) return false;
    if (count > SHADOW_ATTR_MAX) count = SHADOW_ATTR_MAX;
    char key[16];
    snprintf(key, sizeof(key), "a%014llX", (unsigned long long)(ieee & 0x00FFFFFFFFFFFFFFULL));

    // F26: prepend version + count + CRC header (see ShadowBlobHdr).
    const size_t plen = (size_t)count * sizeof(ShadowAttr);
    auto* h   = reinterpret_cast<ShadowBlobHdr*>(s_attr_blob);
    auto* pay = s_attr_blob + sizeof(ShadowBlobHdr);
    memcpy(pay, attrs, plen);
    h->ver   = SHADOW_ATTR_BLOB_VER;
    h->count = count;
    h->_pad  = 0;
    h->crc   = esp_rom_crc32_le(0, pay, plen);

    esp_err_t err = nvs_set_blob(s_nvs, key, s_attr_blob, sizeof(ShadowBlobHdr) + plen);
    if (err != ESP_OK) { ESP_LOGE(TAG, "nvs_set_blob attrs: %s", esp_err_to_name(err)); return false; }
    err = nvs_commit(s_nvs);
    if (err != ESP_OK) ESP_LOGE(TAG, "nvs_commit attrs: %s", esp_err_to_name(err));
    return err == ESP_OK;
}

static bool nvs_save_config(uint64_t ieee, const DeviceConfig* cfg) {
    if (!s_nvs) return false;
    char key[16];
    snprintf(key, sizeof(key), "c%014llX", (unsigned long long)(ieee & 0x00FFFFFFFFFFFFFFULL));
    esp_err_t err = nvs_set_blob(s_nvs, key, cfg, sizeof(DeviceConfig));
    if (err != ESP_OK) { ESP_LOGE(TAG, "nvs_set_blob config: %s", esp_err_to_name(err)); return false; }
    err = nvs_commit(s_nvs);
    if (err != ESP_OK) ESP_LOGE(TAG, "nvs_commit config: %s", esp_err_to_name(err));
    return err == ESP_OK;
}

// F26 (FINDINGS.md): config setters previously hit NVS on every API call.
// Dedupe by CRC of the config bytes — a no-op re-push (e.g. SPA re-sending an
// unchanged config) skips the flash write entirely; a real change still
// persists immediately, so no edit is ever lost. Caller holds s_mutex.
static void persist_config_dedup(DeviceShadowEntry* e) {
    uint32_t crc = esp_rom_crc32_le(0, reinterpret_cast<const uint8_t*>(&e->config),
                                    sizeof(DeviceConfig));
    if (e->cfg_crc_valid && crc == e->cfg_crc) return;   // unchanged → no flash wear
    if (nvs_save_config(e->ieee, &e->config)) {
        e->cfg_crc       = crc;
        e->cfg_crc_valid = true;
    }
}

static bool nvs_load_attrs(uint64_t ieee, ShadowAttr* out, uint8_t* count) {
    if (!s_nvs) return false;
    char key[16];
    snprintf(key, sizeof(key), "a%014llX", (unsigned long long)(ieee & 0x00FFFFFFFFFFFFFFULL));

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
    snprintf(key, sizeof(key), "c%014llX", (unsigned long long)(ieee & 0x00FFFFFFFFFFFFFFULL));
    size_t len = sizeof(DeviceConfig);
    return (nvs_get_blob(s_nvs, key, out, &len) == ESP_OK);
}

static void persist_attrs_throttled(DeviceShadowEntry* e) {
    uint32_t now_s = (uint32_t)(xTaskGetTickCount() / configTICK_RATE_HZ);
    if ((now_s - e->nvs_last_write_s) >= NVS_MIN_INTERVAL_S) {
        if (nvs_save_attrs(e->ieee, e->attrs, e->attr_count)) {
            e->nvs_last_write_s = now_s;
            e->nvs_dirty = false;
        }
    } else {
        e->nvs_dirty = true;
    }
}

static void persist_attrs_force(DeviceShadowEntry* e) {
    if (!e->nvs_dirty && e->attr_count == 0) return;
    uint32_t now_s = (uint32_t)(xTaskGetTickCount() / configTICK_RATE_HZ);
    if (nvs_save_attrs(e->ieee, e->attrs, e->attr_count)) {
        e->nvs_last_write_s = now_s;
        e->nvs_dirty = false;
    }
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
static void emit_zcl_attr(const DeviceShadowEntry* e, const ZclAttribute* attrs, uint8_t count);

// ── Debounce timer callback ──────────────────────────────────────────────

static void debounce_timer_cb(TimerHandle_t xTimer) {
    DeviceShadowEntry* e = static_cast<DeviceShadowEntry*>(pvTimerGetTimerID(xTimer));
    DebounceFireMsg msg{ e->ieee };
    if (xQueueSend(s_debounce_queue, &msg, 0) != pdTRUE) {
        e->debounce_pending_flush = true;
    }
}

// ── Occupancy timeout callback ───────────────────────────────────────────

static void occupancy_timeout_cb(TimerHandle_t timer) {
    DeviceShadowEntry* e = static_cast<DeviceShadowEntry*>(pvTimerGetTimerID(timer));

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (e->config.occupancy_timeout_s == 0) {
        xSemaphoreGive(s_mutex);
        return;
    }

    ZclAttribute synthetic{};
    zcl_attr_set_int(&synthetic, KEY_OCCUPANCY, 0, VAL_BOOL);
    synthetic.cluster = 0x0406;
    synthetic.attr_id = 0x0000;

    upsert_cache(e, &synthetic, 1);
    xSemaphoreGive(s_mutex);

    emit_zcl_attr(e, &synthetic, 1);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    persist_attrs_throttled(e);
    xSemaphoreGive(s_mutex);

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

// ── Emit ZCL_ATTR events for each attr ───────────────────────────────────

static void emit_zcl_attr(const DeviceShadowEntry* e, const ZclAttribute* attrs, uint8_t count) {
    for (uint8_t i = 0; i < count; i++) {
        Event ev{};
        ev.type = EventType::ZCL_ATTR;
        ZclAttrEvent* payload = reinterpret_cast<ZclAttrEvent*>(ev.data);
        payload->ieee     = e->ieee;
        payload->val_type = attrs[i].val_type;
        payload->cluster  = attrs[i].cluster;
        payload->attr_id  = attrs[i].attr_id;
        // memcpy + explicit NUL avoids the -Wstringop-truncation noise
        // -O2 emits when strncpy's dest size equals the source's worst-
        // case length (the next-line NUL keeps it correct, but gcc's
        // analysis can't prove that).
        memcpy(payload->key, attrs[i].key, ATTR_KEY_MAX - 1);
        payload->key[ATTR_KEY_MAX - 1] = '\0';
        if (attrs[i].val_type == VAL_STR) {
            memcpy(payload->str_val, attrs[i].str_val, ATTR_STR_MAX);
        } else {
            payload->int_val = attrs[i].int_val;
        }
        event_bus_publish(ev);
    }
}

// ── Apply pipeline and emit for surviving attrs ───────────────────────────

static void apply_pipeline_and_emit(DeviceShadowEntry* e,
                                     const ZclAttribute* attrs, uint8_t count)
{
    // `ZclAttribute` grew from 12 B to 52 B in the 2026-04-20 attr_keys
    // drop. Three 32-slot local arrays would chew ~5 KB of the zcl_attr
    // task's 8 KB stack — overflow observed on Xiaomi cube interview.
    // Caller holds `s_mutex` for the entire duration of this function so
    // `static` buffers are safe (single-threaded access guaranteed).
    static ZclAttribute filtered[32];
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
        static ZclAttribute bypass[32];
        uint8_t bypass_count = 0;
        static ZclAttribute merge[32];
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
        xTimerReset(e->debounce_timer, 0);

        if (bypass_count > 0) {
            upsert_cache(e, bypass, bypass_count);
            emit_zcl_attr(e, bypass, bypass_count);
        }

    } else if (e->config.throttle_ms > 0) {
        if (!shadow_pipeline_throttle_pass(&e->config, &e->throttle_last_ms, now_ms))
            return;
        upsert_cache(e, filtered, n);
        emit_zcl_attr(e, filtered, n);
        persist_attrs_throttled(e);
    } else {
        upsert_cache(e, filtered, n);
        emit_zcl_attr(e, filtered, n);
        persist_attrs_throttled(e);
    }
}

static void flush_pending_entry_locked(DeviceShadowEntry* e) {
    // Static — caller holds s_mutex, same reason as apply_pipeline_and_emit.
    static ZclAttribute out[32];
    uint8_t n = shadow_pipeline_flush_pending(&e->pending, out, 32);
    if (n > 0) {
        upsert_cache(e, out, n);
        emit_zcl_attr(e, out, n);
        persist_attrs_throttled(e);
    }
    e->debounce_pending_flush = false;
}

// ── task_shadow: handles debounce timer fire events ───────────────────────

static void task_shadow(void* arg) {
    DebounceFireMsg msg{};
    for (;;) {
        bool got_msg = (xQueueReceive(s_debounce_queue, &msg, pdMS_TO_TICKS(100)) == pdTRUE);

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        if (got_msg) {
            DeviceShadowEntry* e = find_entry(msg.ieee);
            if (e) flush_pending_entry_locked(e);
        }

        uint32_t now_s = (uint32_t)(xTaskGetTickCount() / configTICK_RATE_HZ);
        for (uint16_t i = 0; i < s_count; i++) {
            DeviceShadowEntry* e = &s_shadow[i];
            if (e->debounce_pending_flush) flush_pending_entry_locked(e);
            if (e->nvs_dirty && (now_s - e->nvs_last_write_s) >= NVS_MIN_INTERVAL_S) {
                if (nvs_save_attrs(e->ieee, e->attrs, e->attr_count)) {
                    e->nvs_last_write_s = now_s;
                    e->nvs_dirty = false;
                }
            }
        }

        xSemaphoreGive(s_mutex);
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
            nvs_erase_all(s_nvs);
            nvs_set_u8(s_nvs, "ver", NVS_SHADOW_VERSION);
            nvs_commit(s_nvs);
            ESP_LOGW(TAG, "shadow NVS format upgraded v%d — cache cleared", NVS_SHADOW_VERSION);
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
    s_debounce_queue = xQueueCreate(16, sizeof(DebounceFireMsg));
    configASSERT(s_debounce_queue);

    s_shadow = static_cast<DeviceShadowEntry*>(
        heap_caps_calloc(ZAP_MAX_DEVICES, sizeof(DeviceShadowEntry), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    configASSERT(s_shadow);

    xTaskCreatePinnedToCore(task_shadow, "task_shadow", zhac::stack::kDeviceShadow, nullptr, 4, nullptr, 1);
    ESP_LOGI(TAG, "device_shadow init OK");
}

uint16_t device_shadow_restore_from_pool(const ZapDevice* pool, uint16_t count) {
    uint16_t restored = 0;
    for (uint16_t i = 0; i < count; i++) {
        const ZapDevice* d = &pool[i];
        DeviceShadowEntry* e = find_or_create_entry(d->ieee_addr);
        if (!e) continue;
        if (nvs_load_config(d->ieee_addr, &e->config)) {
            // F26: seed the dedupe CRC so an unchanged config re-push after
            // boot doesn't trigger a redundant NVS write.
            e->cfg_crc = esp_rom_crc32_le(0, reinterpret_cast<const uint8_t*>(&e->config),
                                          sizeof(DeviceConfig));
            e->cfg_crc_valid = true;
        }
        uint8_t ac = 0;
        if (nvs_load_attrs(d->ieee_addr, e->attrs, &ac)) e->attr_count = ac;
        // Restore last_seen from synthetic attr → ZapDevice. Deferred via
        // mark_dirty(LOW) so the flush task batches the write outside the
        // boot critical path.
        for (uint8_t j = 0; j < e->attr_count; j++) {
            if (strncmp(e->attrs[j].key, KEY_LAST_SEEN, ATTR_KEY_MAX) == 0) {
                ZapDevice patched = *d;
                patched.last_seen = (uint32_t)e->attrs[j].int_val;
                zap_store_mark_dirty(&patched, ZAP_PERSIST_LOW);
                break;
            }
        }
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
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    DeviceShadowEntry* e = find_or_create_entry(dev->ieee_addr);
    if (!e) { xSemaphoreGive(s_mutex); return; }

    apply_pipeline_and_emit(e, attrs, count);

    // Inject synthetic _last_seen (bypasses pipeline — internal bookkeeping)
    if (e->config.last_seen_enabled) {
        ZclAttribute ls{};
        zcl_attr_set_int(&ls, KEY_LAST_SEEN, (int32_t)time(nullptr), VAL_INT);
        ls.cluster = 0xFFFF;
        ls.attr_id = 0xFFFF;
        upsert_cache(e, &ls, 1);
    }

    xSemaphoreGive(s_mutex);
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

bool device_shadow_set_config(uint64_t ieee, const DeviceConfig* cfg) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    DeviceShadowEntry* e = find_or_create_entry(ieee);
    if (!e) { xSemaphoreGive(s_mutex); return false; }

    flush_pending_entry_locked(e);
    if (e->nvs_dirty) persist_attrs_force(e);

    e->config = *cfg;
    persist_config_dedup(e);
    xSemaphoreGive(s_mutex);
    return true;
}

bool device_shadow_set_throttle_ms(uint64_t ieee, uint32_t throttle_ms) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    DeviceShadowEntry* e = find_or_create_entry(ieee);
    if (!e) { xSemaphoreGive(s_mutex); return false; }
    e->config.throttle_ms = throttle_ms;
    e->throttle_last_ms = 0;   // reset window so the new rate applies immediately
    persist_config_dedup(e);
    xSemaphoreGive(s_mutex);
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
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    DeviceShadowEntry* e = find_entry(ieee);
    if (e) {
        memset(e->attrs, 0, sizeof(e->attrs));
        e->attr_count = 0;

        if (e->occupancy_timer) { xTimerDelete(e->occupancy_timer, 0); e->occupancy_timer = nullptr; }
        if (e->debounce_timer)  { xTimerDelete(e->debounce_timer, 0);  e->debounce_timer  = nullptr; }

        memset(&e->pending, 0, sizeof(e->pending));
        e->throttle_last_ms = 0;
        e->debounce_pending_flush = false;
        e->nvs_dirty = false;

        if (s_nvs) {
            char key[16];
            snprintf(key, sizeof(key), "a%014llX",
                     (unsigned long long)(ieee & 0x00FFFFFFFFFFFFFFULL));
            nvs_erase_key(s_nvs, key);
            nvs_commit(s_nvs);
        }
    }
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "Cleared shadow attr cache ieee=0x%016llX", (unsigned long long)ieee);
}

bool device_shadow_set_debounce_ms(uint64_t ieee, uint32_t debounce_ms) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    DeviceShadowEntry* e = find_or_create_entry(ieee);
    if (!e) { xSemaphoreGive(s_mutex); return false; }

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

    persist_config_dedup(e);
    xSemaphoreGive(s_mutex);
    return true;
}

bool device_shadow_set_occupancy_timeout(uint64_t ieee, uint16_t timeout_s) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    DeviceShadowEntry* e = find_entry(ieee);
    if (!e) { xSemaphoreGive(s_mutex); return false; }

    e->config.occupancy_timeout_s = timeout_s;

    if (timeout_s == 0 && e->occupancy_timer) {
        xTimerDelete(e->occupancy_timer, 0);
        e->occupancy_timer = nullptr;
    }

    persist_config_dedup(e);
    xSemaphoreGive(s_mutex);
    return true;
}
