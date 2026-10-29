// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// components/zap_store/zap_store.cpp
#include "zap_store.h"
#include "zap_clock.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "nvs_checked.h"
#include "nvs_namespaces.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>

static const char* TAG     = "zap_store";
// P2 (FINDINGS §8.5): use the central namespace registry rather than an
// inlined "zap_v0" literal — one place audits every NVS namespace string.
static const char* NVS_NS  = zap_nvs::DEVICE_POOL;
static bool        s_ready = false;

// ── Data partition (review 2026-10-24 WC-04) ─────────────────────────────
// The device pool, and device_shadow's cache (it asks zap_store_data_part()),
// live on `zhac_data` when the firmware has that partition: wired-core's
// 64 KB `nvs` also holds rules, groups, options, sign-in and the coordinator
// pin, and the pool and shadow filled it at a few dozen devices. Firmwares
// without it (main-core, mono) keep everything on `nvs`.
//
// The move is a copy, final only once the image that made it is confirmed:
//   - both namespaces are copied from `nvs`, then a checksum of what was copied
//     ("src") and "done" are set on `zhac_data` ("done" cleared first, so a
//     copy cut short is never taken for a whole one), and the store runs on
//     `zhac_data`;
//   - on an update's trial boot the originals stay: the build before finds
//     them if the update rolls back. Until the move is final, a boot keeps the
//     copy while `nvs` still matches its checksum (nothing wrote `nvs` since,
//     so the copy with this build's writes on top is the newest store) and
//     copies afresh once `nvs` changed (a trial rolled back and the build
//     before wrote to it, or this build came back by USB after that). A
//     rollback whose build wrote nothing keeps the trial's changes, as the
//     Zigbee network does;
//   - zap_store_confirm_move() (the image was marked valid) or a boot that is
//     not a trial makes the move final: "live" on `zhac_data`, beside the data,
//     then "moved" on `nvs`. From then on `zhac_data` holds the store of record:
//     never copied over, never formatted, and leftovers on `nvs` are erased.
//     A mark that will not write only leaves the old copy on `nvs` for a later
//     boot to erase (review 2026-10-29 N-05).
static const char* DATA_PART = "zhac_data";
static const char* MIG_NS    = "zap_mig";   // "done"/"live"/"src" on zhac_data, "moved" on nvs
static const char* s_part    = NVS_DEFAULT_PART_NAME;   // where the pool lives

// F16 (FINDINGS.md): serialise save (flush task) and delete (dispatch task) —
// previously unsynchronised, a latent race on the slot table — and back save
// with an in-RAM IEEE→slot index so a bulk interview is O(n), not O(n²) NVS
// blob scans. The index mirrors the on-disk slot order; delete invalidates it
// (rebuilt lazily on the next save).
static SemaphoreHandle_t s_store_mutex = nullptr;
static uint64_t          s_idx_ieee[ZAP_MAX_DEVICES];
static uint16_t          s_idx_count   = 0;
static bool              s_idx_built   = false;

static inline void store_lock()   { if (s_store_mutex) xSemaphoreTake(s_store_mutex, portMAX_DELAY); }
static inline void store_unlock() { if (s_store_mutex) xSemaphoreGive(s_store_mutex); }

static void dev_key(char* out, uint16_t idx) {
    snprintf(out, 8, "d%04x", idx);
}

static nvs_handle_t open_ns(nvs_open_mode_t mode) {
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(s_part, NVS_NS, mode, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open: %s", esp_err_to_name(err));
        return 0;
    }
    return h;
}

// F16: (re)build the IEEE index from NVS — one O(n) scan. Caller holds the
// store lock and passes an open handle.
static void index_build_locked(nvs_handle_t h) {
    uint16_t count = 0;
    nvs_get_u16(h, "cnt", &count);
    if (count > ZAP_MAX_DEVICES) count = ZAP_MAX_DEVICES;
    s_idx_count = 0;
    for (uint16_t i = 0; i < count; i++) {
        char key[8]; dev_key(key, i);
        ZapDevice tmp{}; size_t sz = sizeof(ZapDevice);
        s_idx_ieee[i] = (nvs_get_blob(h, key, &tmp, &sz) == ESP_OK) ? tmp.ieee_addr : 0;
        s_idx_count = static_cast<uint16_t>(i + 1);
    }
    s_idx_built = true;
}

static int16_t index_find_locked(uint64_t ieee) {
    for (uint16_t i = 0; i < s_idx_count; i++)
        if (s_idx_ieee[i] == ieee) return static_cast<int16_t>(i);
    return -1;
}

// last_seen side-car (zap_store.h): one row per stored device, one blob.
struct __attribute__((packed)) LastSeenRow {
    uint64_t ieee;
    uint32_t ts;
};
static_assert(sizeof(LastSeenRow) == 12);
static const char* LAST_SEEN_KEY = "lseen";
static constexpr size_t LAST_SEEN_CAP = ZAP_MAX_DEVICES * sizeof(LastSeenRow);   // 2.4 KB
static uint32_t    s_ls_crc = 0;   // CRC of the rows on flash, so an unchanged set is not rewritten

static LastSeenRow* last_seen_rows_alloc() {
    void* p = heap_caps_malloc(LAST_SEEN_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return static_cast<LastSeenRow*>(p ? p : std::malloc(LAST_SEEN_CAP));
}

static uint32_t last_seen_crc(const LastSeenRow* rows, size_t n) {
    return esp_rom_crc32_le(0, reinterpret_cast<const uint8_t*>(rows),
                            static_cast<uint32_t>(n * sizeof(LastSeenRow)));
}

// ── CRC32 integrity ──────────────────────────────────────────────────────

// Compute CRC32 over a ZapDevice whose crc32 field is ALREADY zeroed by the
// caller. No defensive stack copy — the save path holds a crc-zeroed copy
// and the verify path zeroes the field around the check. Keeping the copy
// out of here avoids a redundant 522 B stack frame + memcpy per call
// (FINDINGS §8.4).
static inline uint32_t zap_device_crc_zeroed(const ZapDevice* d) {
    return esp_rom_crc32_le(0, reinterpret_cast<const uint8_t*>(d), sizeof(*d));
}

// Compute CRC32 over an arbitrary ZapDevice (crc32 field excluded). Used by
// the load path, where the in-RAM record carries the STORED crc32 that must
// be preserved for the equality check — so this variant zeroes a copy.
static uint32_t zap_device_crc(const ZapDevice* d) {
    ZapDevice tmp;
    memcpy(&tmp, d, sizeof(tmp));
    tmp.crc32 = 0;
    return zap_device_crc_zeroed(&tmp);
}

// Schema version stored in NVS alongside data.
// Bump this constant AND migrate/erase old data whenever ZapDevice layout changes.
// v4 → v5: added CRC32 integrity checking on device records.
// v5 → v6: added lifecycle state fields (interview/support/configure_state,
//          configure_attempts) to ZapDevice; struct grew 518 → 522 bytes.
static constexpr uint16_t ZAP_STORE_SCHEMA_VERSION = 6;

// ── Moving the store to zhac_data ────────────────────────────────────────

// Each value read goes to `dst` when there is one, and into `*crc`, which lets
// a later boot tell whether nvs changed since the copy.
template <typename T, esp_err_t (*Get)(nvs_handle_t, const char*, T*),
          esp_err_t (*Set)(nvs_handle_t, const char*, T)>
static bool copy_num(nvs_handle_t src, const nvs_handle_t* dst, const char* key, uint32_t* crc) {
    T v{};
    if (Get(src, key, &v) != ESP_OK) return false;
    *crc = esp_rom_crc32_le(*crc, reinterpret_cast<const uint8_t*>(&v), sizeof v);
    return !dst || Set(*dst, key, v) == ESP_OK;
}

static bool copy_bytes(nvs_handle_t src, const nvs_handle_t* dst, const char* key, bool str, uint32_t* crc) {
    size_t len = 0;
    if ((str ? nvs_get_str(src, key, nullptr, &len) : nvs_get_blob(src, key, nullptr, &len)) != ESP_OK)
        return false;
    void* buf = heap_caps_malloc(len ? len : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = std::malloc(len ? len : 1);
    if (!buf) return false;
    bool ok = (str ? nvs_get_str(src, key, static_cast<char*>(buf), &len)
                   : nvs_get_blob(src, key, buf, &len)) == ESP_OK;
    if (ok) *crc = esp_rom_crc32_le(*crc, static_cast<const uint8_t*>(buf), len);
    if (ok && dst)
        ok = (str ? nvs_set_str(*dst, key, static_cast<const char*>(buf))
                  : nvs_set_blob(*dst, key, buf, len)) == ESP_OK;
    heap_caps_free(buf);
    return ok;
}

static bool copy_entry(nvs_handle_t src, const nvs_handle_t* dst, const char* key, nvs_type_t type,
                       uint32_t* crc) {
    switch (type) {
    case NVS_TYPE_U8:   return copy_num<uint8_t,  nvs_get_u8,  nvs_set_u8>(src, dst, key, crc);
    case NVS_TYPE_I8:   return copy_num<int8_t,   nvs_get_i8,  nvs_set_i8>(src, dst, key, crc);
    case NVS_TYPE_U16:  return copy_num<uint16_t, nvs_get_u16, nvs_set_u16>(src, dst, key, crc);
    case NVS_TYPE_I16:  return copy_num<int16_t,  nvs_get_i16, nvs_set_i16>(src, dst, key, crc);
    case NVS_TYPE_U32:  return copy_num<uint32_t, nvs_get_u32, nvs_set_u32>(src, dst, key, crc);
    case NVS_TYPE_I32:  return copy_num<int32_t,  nvs_get_i32, nvs_set_i32>(src, dst, key, crc);
    case NVS_TYPE_U64:  return copy_num<uint64_t, nvs_get_u64, nvs_set_u64>(src, dst, key, crc);
    case NVS_TYPE_I64:  return copy_num<int64_t,  nvs_get_i64, nvs_set_i64>(src, dst, key, crc);
    case NVS_TYPE_STR:  return copy_bytes(src, dst, key, true, crc);
    case NVS_TYPE_BLOB: return copy_bytes(src, dst, key, false, crc);
    default:            return false;
    }
}

// Every entry of `ns`, from one partition to the other, or only read when `to`
// is null. Each entry's checksum (namespace, key, type and value) is added to
// `*sum`, so the total does not depend on the order the entries come in.
static bool copy_ns(const char* from, const char* to, const char* ns, uint32_t* sum) {
    nvs_handle_t src = 0, dst = 0;
    const esp_err_t oe = nvs_open_from_partition(from, ns, NVS_READONLY, &src);
    if (oe == ESP_ERR_NVS_NOT_FOUND) return true;   // never written: nothing to copy
    if (oe != ESP_OK) return false;
    if (to && nvs_open_from_partition(to, ns, NVS_READWRITE, &dst) != ESP_OK) {
        nvs_close(src);
        return false;
    }
    bool ok = true;
    unsigned n = 0;
    nvs_iterator_t it = nullptr;
    esp_err_t e = nvs_entry_find(from, ns, NVS_TYPE_ANY, &it);
    while (ok && e == ESP_OK) {
        nvs_entry_info_t info{};
        nvs_entry_info(it, &info);
        uint32_t crc = esp_rom_crc32_le(0, reinterpret_cast<const uint8_t*>(ns), strlen(ns) + 1);
        crc = esp_rom_crc32_le(crc, reinterpret_cast<const uint8_t*>(info.key), strlen(info.key) + 1);
        crc = esp_rom_crc32_le(crc, reinterpret_cast<const uint8_t*>(&info.type), sizeof info.type);
        ok = copy_entry(src, to ? &dst : nullptr, info.key, info.type, &crc);
        if (!ok) ESP_LOGE(TAG, "%s %s/%s failed", to ? "copying" : "reading", ns, info.key);
        *sum += crc;
        n++;
        e = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    if (to) nvs_close(dst);
    nvs_close(src);
    if (ok && to) ESP_LOGI(TAG, "copied %u entries of %s to %s", n, ns, to);
    return ok && e == ESP_ERR_NVS_NOT_FOUND;
}

// The checksum of the two namespaces as they are on nvs. False when they
// cannot be read.
static bool nvs_store_sum(uint32_t* sum) {
    *sum = 0;
    return copy_ns(NVS_DEFAULT_PART_NAME, nullptr, NVS_NS, sum) &&
           copy_ns(NVS_DEFAULT_PART_NAME, nullptr, zap_nvs::SHADOW, sum);
}

static bool erase_ns(const char* part, const char* ns) {
    nvs_handle_t h = 0;
    if (nvs_open_from_partition(part, ns, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t acc = ESP_OK;
    nvs_seq(&acc, nvs_erase_all(h), TAG, "erase_all");
    nvs_seq(&acc, nvs_commit(h), TAG, "commit erase_all");
    nvs_close(h);
    return acc == ESP_OK;
}

static bool mark_get(const char* part, const char* key) {
    nvs_handle_t h = 0;
    uint8_t v = 0;
    if (nvs_open_from_partition(part, MIG_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, key, &v);
        nvs_close(h);
    }
    return v == 1;
}

static bool mark_set(const char* part, const char* key) {
    nvs_handle_t h = 0;
    if (nvs_open_from_partition(part, MIG_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t acc = ESP_OK;
    nvs_seq(&acc, nvs_set_u8(h, key, 1), TAG, "set_u8 move mark");
    nvs_seq(&acc, nvs_commit(h), TAG, "commit move mark");
    nvs_close(h);
    return acc == ESP_OK;
}

static bool mark_clear(const char* part, const char* key) {
    nvs_handle_t h = 0;
    if (nvs_open_from_partition(part, MIG_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    const esp_err_t e = nvs_erase_key(h, key);
    const bool ok = (e == ESP_OK || e == ESP_ERR_NVS_NOT_FOUND) && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

// "src": the checksum of nvs the copy on zhac_data was taken from.
static bool src_get(uint32_t* v) {
    nvs_handle_t h = 0;
    if (nvs_open_from_partition(DATA_PART, MIG_NS, NVS_READONLY, &h) != ESP_OK) return false;
    const bool ok = nvs_get_u32(h, "src", v) == ESP_OK;
    nvs_close(h);
    return ok;
}

static bool src_set(uint32_t v) {
    nvs_handle_t h = 0;
    if (nvs_open_from_partition(DATA_PART, MIG_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t acc = ESP_OK;
    nvs_seq(&acc, nvs_set_u32(h, "src", v), TAG, "set_u32 copy checksum");
    nvs_seq(&acc, nvs_commit(h), TAG, "commit copy checksum");
    nvs_close(h);
    return acc == ESP_OK;
}

// The namespace holds at least one entry.
static bool ns_present(const char* part, const char* ns) {
    nvs_iterator_t it = nullptr;
    const bool any = nvs_entry_find(part, ns, NVS_TYPE_ANY, &it) == ESP_OK;
    nvs_release_iterator(it);
    return any;
}

// The namespace can be read (or was never written) -- false when its
// partition itself is unusable.
static bool ns_readable(const char* part, const char* ns) {
    nvs_handle_t h = 0;
    const esp_err_t e = nvs_open_from_partition(part, ns, NVS_READONLY, &h);
    if (e == ESP_OK) nvs_close(h);
    return e == ESP_OK || e == ESP_ERR_NVS_NOT_FOUND;
}

// The first boot of an update, while it can still roll back to the build
// before it (which reads the store from nvs).
static bool image_on_trial() {
    esp_ota_img_states_t st;
    const esp_partition_t* run = esp_ota_get_running_partition();
    return run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
}

static void erase_old_copy() {
    if (!ns_present(NVS_DEFAULT_PART_NAME, NVS_NS) && !ns_present(NVS_DEFAULT_PART_NAME, zap_nvs::SHADOW)) return;
    if (erase_ns(NVS_DEFAULT_PART_NAME, NVS_NS) && erase_ns(NVS_DEFAULT_PART_NAME, zap_nvs::SHADOW))
        ESP_LOGI(TAG, "old device store on nvs erased (it lives on '%s')", DATA_PART);
    else
        ESP_LOGW(TAG, "the old device store on nvs could not be erased -- retry next boot");
}

// Picks the partition the store lives on, moving it there once (see the top
// of this file). False when the store of record sits on a partition that
// cannot be used: the store then stays shut.
static bool data_part_select() {
    s_part = NVS_DEFAULT_PART_NAME;
    const bool moved = mark_get(NVS_DEFAULT_PART_NAME, "moved");
    if (!esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, DATA_PART)) {
        if (moved) ESP_LOGE(TAG, "the device store lives on '%s', which this build does not have", DATA_PART);
        return !moved;
    }
    // Before the move nvs holds a whole store, so an unusable zhac_data is
    // formatted (this build cannot read what it holds anyway); after it (or with
    // nothing on nvs, so zhac_data may hold the only copy) it is never formatted.
    // ponytail: with no mark yet, a zhac_data that will not open cannot tell a
    // first move from a kept update whose marks failed (review 2026-10-29 N-05),
    // so this boot falls back to nvs or formats, and what was saved since the
    // copy is lost. That takes a second fault on the same partition; fail closed
    // here (store shut) if it ever matters more than a first move onto a broken
    // zhac_data needing a storage reset.
    const bool before_move = !moved && ns_present(NVS_DEFAULT_PART_NAME, NVS_NS);
    esp_err_t e = nvs_flash_init_partition(DATA_PART);
    if (before_move && (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND)) {
        ESP_LOGW(TAG, "'%s' not usable (%s) -- formatting it, the store is still on nvs",
                 DATA_PART, esp_err_to_name(e));
        e = nvs_flash_erase_partition(DATA_PART);
        if (e == ESP_OK) e = nvs_flash_init_partition(DATA_PART);
    }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "'%s' not usable: %s%s", DATA_PART, esp_err_to_name(e),
                 before_move ? " -- the store stays on nvs"
                             : " -- it may hold the device store, which stays shut (not erased)");
        if (!before_move) s_part = DATA_PART;   // so the shadow cannot fill nvs meanwhile
        return before_move;
    }
    if (moved || mark_get(DATA_PART, "live")) {
        s_part = DATA_PART;
        if (!mark_get(DATA_PART, "done")) {
            ESP_LOGE(TAG, "'%s' lost the device store (no move mark) -- store shut; reset storage", DATA_PART);
            return false;
        }
        zap_store_confirm_move();   // either mark may be missing: nvs was erased, or a write failed
        // Confirmed at run time, or left by an older build since. Not on a
        // trial boot: if this update rolls back, that older build wants them.
        if (!image_on_trial()) erase_old_copy();
        return true;
    }
    // Not moved: nvs holds the store of record. If nvs itself cannot be read,
    // leave zhac_data alone (the store then fails to open and stays shut).
    if (!ns_readable(NVS_DEFAULT_PART_NAME, NVS_NS)) return true;
    // Until the move is final the copy counts only while nvs is still what it
    // was taken from. Then nothing wrote nvs since, and the copy with this
    // build's writes on top is the newest store, also after a restart that came
    // before a mark would write (review 2026-10-29 N-05). Once nvs changed (a
    // trial rolled back and the build before wrote to it, or this build came
    // back by USB after that) the copy is stale and is taken afresh. "done"
    // alone cannot tell the two apart; the checksum kept with the copy can.
    uint32_t now = 0, then = 0;
    const bool read = nvs_store_sum(&now);
    const bool copy = mark_get(DATA_PART, "done") && src_get(&then);
    if (copy && !read) {
        // Whether the copy is current cannot be told this boot, and it may hold
        // writes nvs lacks. Running on nvs would let this boot's writes make it
        // look stale at the next one, so the store stays shut (fix review S-1).
        ESP_LOGE(TAG, "the device store on nvs cannot be read whole -- store shut this boot");
        s_part = DATA_PART;   // so the shadow cannot write nvs meanwhile
        return false;
    }
    // An nvs with no store at all was wiped by a tool (this store always keeps
    // its schema there, and a storage reset erases zhac_data first). It is not
    // a newer store: the copy stays, as it would after a final move.
    if (copy && (then == now || !ns_present(NVS_DEFAULT_PART_NAME, NVS_NS))) {
        ESP_LOGI(TAG, "the copy on '%s' is current (nvs unchanged since it was taken)", DATA_PART);
    } else {
        uint32_t sum = 0;
        const bool ok = read && mark_clear(DATA_PART, "done") &&
                        erase_ns(DATA_PART, NVS_NS) && erase_ns(DATA_PART, zap_nvs::SHADOW) &&
                        copy_ns(NVS_DEFAULT_PART_NAME, DATA_PART, NVS_NS, &sum) &&
                        copy_ns(NVS_DEFAULT_PART_NAME, DATA_PART, zap_nvs::SHADOW, &sum) &&
                        src_set(sum) && mark_set(DATA_PART, "done");
        if (!ok) {
            ESP_LOGE(TAG, "copying the device store to '%s' failed -- staying on nvs, retry next boot", DATA_PART);
            return true;
        }
    }
    s_part = DATA_PART;
    if (image_on_trial()) {
        ESP_LOGI(TAG, "device store on '%s'; the copy on nvs stays until this update is confirmed", DATA_PART);
        return true;
    }
    if (!zap_store_confirm_move()) {   // this image cannot roll back
        // Still on zhac_data: the store does not write nvs from there, so the
        // next boot finds the copy current and keeps it (above).
        ESP_LOGW(TAG, "could not mark the move to '%s' final -- the copy on nvs stays, retry next boot", DATA_PART);
        return true;
    }
    erase_old_copy();
    ESP_LOGI(TAG, "device store moved to '%s'", DATA_PART);
    return true;
}

bool zap_store_confirm_move() {
    if (s_part != DATA_PART) return true;   // the store is on nvs: nothing to confirm
    // With the data first: an nvs that cannot be read later must not make a
    // trial boot copy over the store of record.
    if (!mark_get(DATA_PART, "live") && !mark_set(DATA_PART, "live"))
        ESP_LOGW(TAG, "could not mark the move to '%s' final", DATA_PART);
    if (!mark_get(NVS_DEFAULT_PART_NAME, "moved")) mark_set(NVS_DEFAULT_PART_NAME, "moved");
    return mark_get(DATA_PART, "live") || mark_get(NVS_DEFAULT_PART_NAME, "moved");
}

bool zap_store_flag_get(const char* key) {
    uint8_t v = 0;
    nvs_handle_t h = 0;
    if (nvs_open_from_partition(s_part, NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, key, &v);
        nvs_close(h);
    }
    return v == 1;
}

bool zap_store_flag_set(const char* key) {
    if (!s_ready) return false;
    nvs_handle_t h = open_ns(NVS_READWRITE);
    if (!h) return false;
    esp_err_t acc = ESP_OK;
    nvs_seq(&acc, nvs_set_u8(h, key, 1), TAG, "set_u8 flag");
    nvs_seq(&acc, nvs_commit(h), TAG, "commit flag");
    nvs_close(h);
    return acc == ESP_OK;
}

// A delete cut after its first write leaves the last record twice, in the
// deleted slot and at the tail (review 2026-09 CC-01): finish it by dropping
// the tail. Caller holds the store lock.
static void repair_interrupted_delete(nvs_handle_t h) {
    uint16_t count = 0;
    nvs_get_u16(h, "cnt", &count);
    if (count < 2 || count > ZAP_MAX_DEVICES) return;
    index_build_locked(h);
    const uint64_t tail = s_idx_ieee[count - 1];
    for (uint16_t i = 0; tail && i + 1 < count; i++) {
        if (s_idx_ieee[i] != tail) continue;
        char key[8]; dev_key(key, static_cast<uint16_t>(count - 1));
        esp_err_t acc = ESP_OK;
        nvs_seq(&acc, nvs_set_u16(h, "cnt", static_cast<uint16_t>(count - 1)), TAG, "set_u16 cnt");
        if (acc == ESP_OK) nvs_erase_key(h, key);
        ESP_LOGW(TAG, "finished a device delete cut short by a restart (ieee=0x%016llx)",
                 (unsigned long long)tail);
        break;
    }
    s_idx_built = false;
}

void zap_store_init() {
    if (!s_store_mutex) s_store_mutex = xSemaphoreCreateMutex();   // F16
    s_ready = false;
    // NVS itself is the firmware's to bring up (every app_main does it first),
    // and never this store's to erase (review 2026-10-24 PB-01): wired-core
    // boots "locked and empty" when NVS cannot start, and this used to wipe
    // the whole partition behind its back. A store that cannot be opened stays
    // shut -- nothing is loaded or saved, and zap_store_is_ready() says so.
    if (!data_part_select()) return;

    store_lock();
    nvs_handle_t h = open_ns(NVS_READWRITE);
    if (!h) {
        store_unlock();
        ESP_LOGE(TAG, "device store unavailable -- running without it");
        return;
    }
    // Schema version check: detect struct layout changes between firmware versions.
    bool ok = true;
    uint16_t stored_ver = 0;
    if (nvs_get_u16(h, "schema_ver", &stored_ver) == ESP_OK) {
        if (stored_ver > ZAP_STORE_SCHEMA_VERSION) {
            // Written by a newer firmware: this image is a downgrade. Keep the
            // store for that firmware instead of wiping it.
            ESP_LOGE(TAG, "device store has schema %u, newer than this firmware's %u -- "
                          "not loaded and not erased (update again, or reset storage)",
                     stored_ver, ZAP_STORE_SCHEMA_VERSION);
            ok = false;
        } else if (stored_ver != ZAP_STORE_SCHEMA_VERSION) {
            ESP_LOGW(TAG, "NVS schema version mismatch: stored=%u current=%u — "
                     "erasing device store",
                     stored_ver, ZAP_STORE_SCHEMA_VERSION);
            esp_err_t acc = ESP_OK;
            nvs_seq(&acc, nvs_erase_all(h), TAG, "erase_all devpool");
            if (acc == ESP_OK) {
                // Version marker only after a clean wipe — writing the
                // new version over a failed erase would let stale-layout
                // blobs sit behind the current marker.
                nvs_seq(&acc, nvs_set_u16(h, "schema_ver",
                                          ZAP_STORE_SCHEMA_VERSION),
                        TAG, "set_u16 schema_ver");
                nvs_seq(&acc, nvs_commit(h), TAG, "commit schema_ver");
            }
            if (acc != ESP_OK) {
                ESP_LOGE(TAG, "schema wipe incomplete — will retry next boot");
                ok = false;
            }
        }
    } else {
        // First boot with versioning — write current version
        esp_err_t acc = ESP_OK;
        nvs_seq(&acc, nvs_set_u16(h, "schema_ver", ZAP_STORE_SCHEMA_VERSION),
                TAG, "set_u16 schema_ver");
        nvs_seq(&acc, nvs_commit(h), TAG, "commit schema_ver");
    }
    if (ok) repair_interrupted_delete(h);
    nvs_close(h);
    store_unlock();
    if (!ok) return;

    s_ready = true;
    ESP_LOGI(TAG, "zap_store init OK (schema=%u, on %s)", ZAP_STORE_SCHEMA_VERSION, s_part);
}

const char* zap_store_data_part() { return s_part; }

bool zap_store_is_ready() { return s_ready; }

// ── Save device with CRC32 ───────────────────────────────────────────────
// Resolves the slot via the in-RAM IEEE index (built once from NVS), writes
// with a CRC32 checksum. F16 (FINDINGS.md): the store mutex serialises this
// with zap_store_delete_device (cross-task — flush vs dispatch), and the index
// removes the old per-save O(n) NVS scan (a bulk interview was O(n²)). The
// never-implemented A/B-snapshot + journal scheme was removed 2026-05-29; NVS
// is the store.

// The save itself; the caller holds store_lock.
static bool save_locked(const ZapDevice* dev) {
    nvs_handle_t h = open_ns(NVS_READWRITE);
    if (!h) return false;

    uint16_t count = 0;
    nvs_get_u16(h, "cnt", &count);

    // F16 (FINDINGS.md): resolve the slot via the in-RAM IEEE index instead of
    // an O(n) NVS blob scan per save (which made a bulk interview O(n²)). Built
    // once from NVS, kept in sync below; delete invalidates it.
    char key[8];
    if (!s_idx_built) index_build_locked(h);
    int16_t found_idx = index_find_locked(dev->ieee_addr);

    // Capacity guard (FINDINGS §8.1): a NEW device (found_idx < 0) at a full
    // store would take idx = count = ZAP_MAX_DEVICES, write blob "d00c8",
    // and bump cnt to 201 — but the index array and every load/delete loop
    // cap at ZAP_MAX_DEVICES, so that slot is unreferenced. Each later save
    // of the same IEEE re-misses the index, re-appends another 522 B blob at
    // an ever-higher idx, and re-bumps cnt: unbounded NVS growth until the
    // partition is exhausted. Reject BEFORE any write or count bump. An
    // existing device (found_idx >= 0) is always an in-place rewrite and is
    // never rejected, even at capacity.
    if (found_idx < 0 && count >= ZAP_MAX_DEVICES) {
        ESP_LOGE(TAG, "device store FULL (%u/%u) — rejecting save ieee=0x%016llx",
                 count, (unsigned)ZAP_MAX_DEVICES,
                 (unsigned long long)dev->ieee_addr);
        nvs_close(h);
        return false;
    }

    uint16_t idx = (found_idx >= 0) ? static_cast<uint16_t>(found_idx) : count;

    // Compute CRC32 before saving (zero the field so it's excluded from the
    // checksum, then CRC the zeroed copy in place — no second stack copy).
    ZapDevice saved_dev;
    memcpy(&saved_dev, dev, sizeof(saved_dev));
    saved_dev.crc32 = 0;
    saved_dev.crc32 = zap_device_crc_zeroed(&saved_dev);

    // For a new device, bump "cnt" BEFORE writing the blob so that a power
    // loss between the two writes leaves a referenced-but-unwritten slot
    // (load_devices skips it via the nvs_get_blob != ESP_OK branch or the
    // CRC32 check) rather than a written-but-unreferenced blob (which the
    // load loop would never read because the count stops short). Without
    // this order the new-device record is silently lost on crash —
    // device rejoins + re-interviews, but friendly name + configure state
    // are gone until the new interview lands.
    dev_key(key, idx);
    if (found_idx < 0) {
        esp_err_t cnt_err = nvs_set_u16(h, "cnt", count + 1);
        if (cnt_err != ESP_OK) {
            ESP_LOGE(TAG, "nvs_set_u16 cnt: %s", esp_err_to_name(cnt_err));
            nvs_close(h);
            return false;
        }
    }
    esp_err_t err = nvs_set_blob(h, key, &saved_dev, sizeof(saved_dev));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_blob: %s", esp_err_to_name(err));
        nvs_close(h);
        return false;
    }

    esp_err_t acc = ESP_OK;
    nvs_seq(&acc, nvs_commit(h), TAG, "commit devpool save");
    nvs_close(h);

    // F16: keep the index in sync on success. (nvs_commit is a no-op on IDF:
    // each write above was durable when it returned.)
    if (acc == ESP_OK && idx < ZAP_MAX_DEVICES) {
        s_idx_ieee[idx] = dev->ieee_addr;
        if (idx >= s_idx_count) s_idx_count = static_cast<uint16_t>(idx + 1);
    }

    // Success log gated on the commit — a failed save must not read as
    // "Device saved" in the log while the function returns false.
    if (acc == ESP_OK)
        ESP_LOGI(TAG, "Device saved idx=%u ieee=0x%016llx", idx, dev->ieee_addr);
    return acc == ESP_OK;
}

bool zap_store_save_device(const ZapDevice* dev) {
    if (!dev || !s_ready) return false;
    store_lock();   // F16: serialise with delete + protect the IEEE index
    const bool ok = save_locked(dev);
    store_unlock();
    return ok;
}

// The pool's copy, read and written under the store lock. A hard delete takes
// the device out of the pool before it deletes the record, so this either
// writes before that delete (which then removes the record) or finds the
// device gone and writes nothing: a copy taken before the delete can no
// longer be written after it (review 2026-10-28 R-11).
bool zap_store_save_live(uint64_t ieee, ZapStoreSnapshotCb snapshot, bool* gone) {
    if (gone) *gone = false;
    if (!snapshot || !s_ready) return false;
    ZapDevice dev{};
    bool ok = false;
    store_lock();
    if (!snapshot(ieee, &dev)) {
        if (gone) *gone = true;
    } else {
        ok = save_locked(&dev);
    }
    store_unlock();
    return ok;
}

// ── Delete device — three writes, safe at every cut ──────────────────────
// nvs_commit() is a no-op on IDF (every set and erase is durable when it
// returns), so the old erase-everything-then-rewrite delete lost the whole
// pool to a power cut in its window (review 2026-09 CC-01). Now: (1) the last
// record is copied into the deleted slot, (2) the count shrinks, (3) the old
// tail key is erased. A cut after (1) leaves the tail record twice, which
// zap_store_init() finishes (repair_interrupted_delete); a cut after (2)
// leaves an unreferenced key that the next new device overwrites.

bool zap_store_delete_device(uint64_t ieee) {
    bool      ok    = false;
    uint16_t  count = 0;
    ZapDevice* tail = nullptr;
    store_lock();   // F16: serialise with save + protect the IEEE index
    do {
        if (!s_ready || ieee == 0) break;
        nvs_handle_t h = open_ns(NVS_READWRITE);
        if (!h) break;
        nvs_get_u16(h, "cnt", &count);
        if (count > ZAP_MAX_DEVICES) count = ZAP_MAX_DEVICES;
        if (!s_idx_built) index_build_locked(h);
        const int16_t found = index_find_locked(ieee);
        if (found < 0 || found >= count) { nvs_close(h); break; }

        const uint16_t last = static_cast<uint16_t>(count - 1);
        char key[8];
        esp_err_t acc = ESP_OK;
        if (found != last) {
            // ZapDevice is ~522 B: off the caller's stack, PSRAM first.
            tail = static_cast<ZapDevice*>(heap_caps_malloc(sizeof(ZapDevice),
                                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (!tail) tail = static_cast<ZapDevice*>(std::malloc(sizeof(ZapDevice)));
            if (!tail) { nvs_close(h); break; }
            size_t sz = sizeof(ZapDevice);
            dev_key(key, last);
            const esp_err_t ge = nvs_get_blob(h, key, tail, &sz);
            dev_key(key, static_cast<uint16_t>(found));
            if (ge == ESP_OK && tail->ieee_addr != ieee) {
                nvs_seq(&acc, nvs_set_blob(h, key, tail, sizeof(ZapDevice)), TAG, "set_blob devpool");
            } else {
                // The tail is a hole (a load skips those) or this same device:
                // leave a hole here instead.
                nvs_seq(&acc, nvs_erase_key(h, key), TAG, "erase_key devpool");
            }
        }
        if (acc == ESP_OK) nvs_seq(&acc, nvs_set_u16(h, "cnt", last), TAG, "set_u16 cnt");
        if (acc == ESP_OK) {
            dev_key(key, last);
            const esp_err_t ee = nvs_erase_key(h, key);
            if (ee != ESP_ERR_NVS_NOT_FOUND) nvs_seq(&acc, ee, TAG, "erase_key devpool tail");
        }
        nvs_close(h);
        if (acc != ESP_OK) break;
        count = last;
        ok = true;
    } while (0);

    s_idx_built = false;   // F16: slot order changed — index rebuilt on next save
    store_unlock();
    heap_caps_free(tail);
    if (ok) {
        ESP_LOGI(TAG, "Device deleted ieee=0x%016llx remaining=%u",
                 (unsigned long long)ieee, count);
    }
    return ok;
}

// ── Load devices with CRC32 validation ───────────────────────────────────
// Each loaded device is checked for CRC integrity. Corrupted entries are
// skipped and logged — the system continues with the remaining valid devices.

uint16_t zap_store_load_devices(ZapDevice* pool, uint16_t max_count) {
    // FINDINGS §8.3: validate the destination, and take the store lock so the
    // multi-blob scan is consistent vs a concurrent save/delete once the flush
    // task is running (load is normally a cold boot path, but the flush task
    // can write before the boot relist completes). Lock order: the flush
    // layer (zap_store_flush.cpp) releases its own s_mtx before calling into
    // save/delete; store_lock comes before the pool lock (zap_store_save_live
    // reads the pool under it). zigbee_pool_restore_persisted calls this while
    // holding the pool lock -- the other way round -- which is safe only at
    // boot, before any deferred write is queued (fix review N2).
    if (!pool || max_count == 0) {
        ESP_LOGE(TAG, "load_devices: null pool or zero capacity");
        return 0;
    }
    if (!s_ready) return 0;

    store_lock();
    nvs_handle_t h = open_ns(NVS_READONLY);
    if (!h) { store_unlock(); return 0; }

    uint16_t count = 0;
    nvs_get_u16(h, "cnt", &count);
    if (count > max_count) count = max_count;

    uint16_t loaded = 0;
    for (uint16_t i = 0; i < count; i++) {
        char key[8]; dev_key(key, i);
        size_t sz = sizeof(ZapDevice);
        if (nvs_get_blob(h, key, &pool[loaded], &sz) != ESP_OK) continue;

        // CRC32 validation
        uint32_t expected = zap_device_crc(&pool[loaded]);
        if (pool[loaded].crc32 != expected) {
            ESP_LOGE(TAG, "CRC32 mismatch idx=%u ieee=0x%016llx "
                     "stored=0x%08lx calc=0x%08lx — skipping",
                     i, (unsigned long long)pool[loaded].ieee_addr,
                     (unsigned long)pool[loaded].crc32, (unsigned long)expected);
            continue;
        }
        // One record per device: the lower slot wins if an interrupted delete
        // left a second copy (init normally finishes it first).
        bool dup = false;
        for (uint16_t j = 0; j < loaded && !dup; j++) dup = pool[j].ieee_addr == pool[loaded].ieee_addr;
        if (dup) continue;

        loaded++;
    }

    // last_seen side-car: raise each record to the newer value saved there. A
    // blob of the wrong size is ignored; the next save rewrites it.
    LastSeenRow* rows = loaded ? last_seen_rows_alloc() : nullptr;
    size_t len = LAST_SEEN_CAP;
    if (rows && nvs_get_blob(h, LAST_SEEN_KEY, rows, &len) == ESP_OK &&
        len % sizeof(LastSeenRow) == 0) {
        const size_t n = len / sizeof(LastSeenRow);
        for (uint16_t i = 0; i < loaded; i++) {
            for (size_t j = 0; j < n; j++) {
                if (rows[j].ieee != pool[i].ieee_addr) continue;
                if (zap_clock_is_set(rows[j].ts) && rows[j].ts > pool[i].last_seen)
                    pool[i].last_seen = rows[j].ts;
                break;
            }
        }
        s_ls_crc = last_seen_crc(rows, n);
    }
    heap_caps_free(rows);

    nvs_close(h);
    store_unlock();
    ESP_LOGI(TAG, "Loaded %u devices from NVS", loaded);
    return loaded;
}

bool zap_store_save_last_seen(ZapStoreSnapshotCb snap) {
    if (!snap) return true;   // no live pool to read yet
    if (!s_ready) return false;
    LastSeenRow* rows = last_seen_rows_alloc();
    if (!rows) return false;

    // The stored devices, copied under the store lock. Their live values are
    // read after it is released: `snap` takes the pool lock, and the boot-time
    // restore takes the two the other way round, so this loop holds one at a time.
    uint16_t n = 0;
    store_lock();
    if (!s_idx_built) {
        if (nvs_handle_t h = open_ns(NVS_READONLY)) {
            index_build_locked(h);
            nvs_close(h);
        }
    }
    for (uint16_t i = 0; i < s_idx_count; i++)
        if (s_idx_ieee[i]) rows[n++].ieee = s_idx_ieee[i];
    store_unlock();

    // A device gone from the pool, or never seen with a set clock, has no row.
    uint16_t m = 0;
    for (uint16_t i = 0; i < n; i++) {
        ZapDevice d{};
        if (snap(rows[i].ieee, &d) && zap_clock_is_set(d.last_seen))
            rows[m++] = LastSeenRow{d.ieee_addr, d.last_seen};
    }

    bool ok = true;
    const uint32_t crc = last_seen_crc(rows, m);
    store_lock();
    if (m > 0 && crc != s_ls_crc) {
        esp_err_t acc = ESP_FAIL;
        if (nvs_handle_t h = open_ns(NVS_READWRITE)) {
            acc = ESP_OK;
            nvs_seq(&acc, nvs_set_blob(h, LAST_SEEN_KEY, rows, m * sizeof(LastSeenRow)),
                    TAG, "set_blob lseen");
            nvs_seq(&acc, nvs_commit(h), TAG, "commit lseen");
            nvs_close(h);
        }
        ok = acc == ESP_OK;
        if (ok) s_ls_crc = crc;
    }
    store_unlock();
    heap_caps_free(rows);
    return ok;
}

// ── Uplink selector ───────────────────────────────────────────────────────
// Task 9 (RainMaker bridge plan): persistent accessor pair backing the
// runtime uplink choice (custom_mqtt | none | rainmaker). One byte in the
// existing device-pool namespace (NVS_NS) — deliberately not a new
// namespace for a single flag. Consumed by later tasks that decide which
// cloud uplink connects at boot.

zhac_uplink_t zhac_uplink_get(void) {
    uint8_t v = (uint8_t)ZHAC_UPLINK_CUSTOM_MQTT;
    nvs_handle_t h = open_ns(NVS_READONLY);
    if (h) {
        // Absent key: ESP-IDF leaves `v` untouched on ESP_ERR_NVS_NOT_FOUND,
        // so it keeps the preset default above — same ignore-the-return-value
        // idiom this file already uses for "cnt" in index_build_locked() /
        // zap_store_save_device() / zap_store_load_devices().
        nvs_get_u8(h, "uplink", &v);
        nvs_close(h);
    }
    // Out-of-range (e.g. a downgrade after a future enum value shipped, or a
    // corrupted byte) falls back to the same backward-compat default as an
    // absent key.
    if (v > (uint8_t)ZHAC_UPLINK_RAINMAKER) v = (uint8_t)ZHAC_UPLINK_CUSTOM_MQTT;
    return (zhac_uplink_t)v;
}

esp_err_t zhac_uplink_set(zhac_uplink_t u) {
    nvs_handle_t h = open_ns(NVS_READWRITE);
    if (!h) return ESP_FAIL;   // open_ns() already logged the specific NVS error

    esp_err_t acc = ESP_OK;
    nvs_seq(&acc, nvs_set_u8(h, "uplink", (uint8_t)u), TAG, "set_u8 uplink");
    nvs_seq(&acc, nvs_commit(h),                        TAG, "commit uplink");
    nvs_close(h);
    return acc;
}

