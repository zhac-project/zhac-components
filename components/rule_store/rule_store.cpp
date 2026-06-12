// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "rule_store.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "nvs_checked.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <cstdio>
#include <cstring>

static const char* TAG    = "rule_store";
static const char* NVS_NS = "zap_rules";

// Bump when RuleSlot layout changes between firmware versions.
static constexpr uint16_t RULE_STORE_SCHEMA_VERSION = 1;

// Cached handle — opened once at init, kept open for the lifetime of the firmware.
static nvs_handle_t s_nvs = 0;

// Mutex protects all rule_store operations.  The NVS handle is shared, so
// concurrent save/load/delete calls must be serialised.
static SemaphoreHandle_t s_mutex = nullptr;

// Compute CRC32 over a RuleSlot with the crc32 field zeroed.
static uint32_t rule_slot_crc(const RuleSlot* s) {
    RuleSlot tmp;
    memcpy(&tmp, s, sizeof(tmp));
    tmp.crc32 = 0;
    return esp_rom_crc32_le(0, reinterpret_cast<const uint8_t*>(&tmp), sizeof(tmp));
}

// ── Internal (unlocked) helpers — caller must hold s_mutex ───────────────

static bool rule_store_save_unlocked(const RuleSlot* slot) {
    char key[8];
    snprintf(key, sizeof(key), "r_%04X", slot->rule_id);
    RuleSlot tmp;
    memcpy(&tmp, slot, sizeof(tmp));
    tmp.crc32 = rule_slot_crc(&tmp);
    esp_err_t err = nvs_set_blob(s_nvs, key, &tmp, sizeof(RuleSlot));
    if (err == ESP_OK) err = nvs_commit(s_nvs);
    if (err != ESP_OK) ESP_LOGE(TAG, "rule_store_save: %s", esp_err_to_name(err));
    return (err == ESP_OK);
}

static bool rule_store_load_unlocked(uint16_t rule_id, RuleSlot* out) {
    char key[8];
    snprintf(key, sizeof(key), "r_%04X", rule_id);
    size_t len = sizeof(RuleSlot);
    esp_err_t err = nvs_get_blob(s_nvs, key, out, &len);
    if (err != ESP_OK) return false;
    // Erase corrupt / stale-schema entries so we don't spam the log on
    // every load. A blob whose size doesn't match the current RuleSlot
    // layout is always unrecoverable (schema bump without version bump);
    // a CRC mismatch usually means torn flash write.
    if (len != sizeof(RuleSlot)) {
        ESP_LOGW(TAG, "size mismatch rule_id=0x%04x (%u != %u) — erasing",
                 rule_id, (unsigned)len, (unsigned)sizeof(RuleSlot));
        // Best-effort cleanup — a failed erase only means the corrupt blob
        // re-logs on the next load; nullptr acc = per-op logging only.
        nvs_seq(nullptr, nvs_erase_key(s_nvs, key), TAG, "erase_key corrupt rule");
        nvs_seq(nullptr, nvs_commit(s_nvs), TAG, "commit corrupt-rule erase");
        return false;
    }
    if (out->crc32 != rule_slot_crc(out)) {
        ESP_LOGW(TAG, "CRC32 mismatch rule_id=0x%04x — erasing", rule_id);
        nvs_seq(nullptr, nvs_erase_key(s_nvs, key), TAG, "erase_key corrupt rule");
        nvs_seq(nullptr, nvs_commit(s_nvs), TAG, "commit corrupt-rule erase");
        return false;
    }
    return true;
}

// ── Public API ───────────────────────────────────────────────────────────

void rule_store_init() {
    s_mutex = xSemaphoreCreateMutex();
    configASSERT(s_mutex);

    if (nvs_open(NVS_NS, NVS_READWRITE, &s_nvs) != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed — rule_store unavailable");
        s_nvs = 0;
        return;
    }
    uint16_t stored_ver = 0;
    if (nvs_get_u16(s_nvs, "schema_ver", &stored_ver) == ESP_OK) {
        if (stored_ver != RULE_STORE_SCHEMA_VERSION) {
            ESP_LOGE(TAG, "rule_store schema mismatch stored=%u current=%u — wiping",
                     stored_ver, RULE_STORE_SCHEMA_VERSION);
            esp_err_t acc = ESP_OK;
            nvs_seq(&acc, nvs_erase_all(s_nvs), TAG, "erase_all rules");
            if (acc == ESP_OK) {
                // Version marker only after a clean wipe (see zap_store).
                nvs_seq(&acc, nvs_set_u16(s_nvs, "schema_ver",
                                          RULE_STORE_SCHEMA_VERSION),
                        TAG, "set_u16 schema_ver");
                nvs_seq(&acc, nvs_commit(s_nvs), TAG, "commit schema_ver");
            }
            // A failed marker write means this wipe repeats every boot —
            // that was silent before; now each failing op logs above.
            if (acc != ESP_OK)
                ESP_LOGE(TAG, "schema wipe incomplete — will re-wipe next boot");
        }
    } else {
        esp_err_t acc = ESP_OK;
        nvs_seq(&acc, nvs_set_u16(s_nvs, "schema_ver", RULE_STORE_SCHEMA_VERSION),
                TAG, "set_u16 schema_ver");
        nvs_seq(&acc, nvs_commit(s_nvs), TAG, "commit schema_ver");
    }
    ESP_LOGI(TAG, "rule_store ready (namespace=%s, schema=%u)", NVS_NS, RULE_STORE_SCHEMA_VERSION);
}

bool rule_store_save(const RuleSlot* slot) {
    if (!s_nvs) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool ok = rule_store_save_unlocked(slot);
    xSemaphoreGive(s_mutex);
    return ok;
}

// Writeback-overlay forward decl. Returns true on WRITE hit (out filled).
// On TOMBSTONE hit, returns false and (if non-NULL) sets *out_tombstoned.
extern "C" bool rule_store_load_overlay(uint16_t rule_id, RuleSlot* out,
                                         bool* out_tombstoned);

bool rule_store_load(uint16_t rule_id, RuleSlot* out) {
    // Check PSRAM overlay first so a just-saved rule is readable before
    // its NVS commit lands. F-02: a tombstoned but not-yet-flushed rule
    // must NOT fall through to NVS, otherwise the deleted rule reappears
    // until the next flush tick (up to ~5 s) and permanently on power-cut.
    bool tombstoned = false;
    if (rule_store_load_overlay(rule_id, out, &tombstoned)) return true;
    if (tombstoned) return false;
    if (!s_nvs) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool ok = rule_store_load_unlocked(rule_id, out);
    xSemaphoreGive(s_mutex);
    return ok;
}

// Tri-state delete (P1-T8, T9 follow-up). Returns:
//   ESP_OK                 — erased and committed;
//   ESP_ERR_NVS_NOT_FOUND  — nothing stored under the key (a tombstone
//                            for a never-committed rule settles on this);
//   ESP_ERR_INVALID_STATE  — store unavailable (nvs_open failed at init);
//   anything else          — erase/commit failure, caller must retry.
// The old bool API collapsed not-found and erase-failure into `false`,
// which let rule_store_flush settle a tombstone whose erase genuinely
// failed — the deleted rule resurrected from NVS on reboot.
// extern "C" so rule_store_flush.cpp declares it without a header round
// trip (same pattern as the overlay hooks above).
extern "C" esp_err_t rule_store_delete_err(uint16_t rule_id) {
    if (!s_nvs) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    char key[8];
    snprintf(key, sizeof(key), "r_%04X", rule_id);
    esp_err_t err = nvs_erase_key(s_nvs, key);
    if (err == ESP_OK) err = nvs_commit(s_nvs);
    xSemaphoreGive(s_mutex);
    // not-found is an expected outcome (delete of a never-persisted rule),
    // not an error — don't spam the log for it.
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
        ESP_LOGE(TAG, "rule_store_delete: %s", esp_err_to_name(err));
    return err;
}

bool rule_store_delete(uint16_t rule_id) {
    return rule_store_delete_err(rule_id) == ESP_OK;  // not-found stays false
}

// Overlay iteration callback: removes tombstoned rules from the NVS
// snapshot and overwrites any rule whose dirty-copy is newer.
extern "C" void rule_store_foreach_dirty(
    void (*cb)(uint16_t rule_id, bool tombstoned, const RuleSlot* slot, void* ctx),
    void* ctx);

namespace {
struct MergeCtx {
    RuleSlot* out;
    uint16_t* count;
    uint16_t  max_count;
};
void merge_cb(uint16_t rule_id, bool tombstoned, const RuleSlot* slot, void* vctx) {
    auto* c = static_cast<MergeCtx*>(vctx);
    // Find existing entry with same rule_id in out[].
    int found = -1;
    for (uint16_t i = 0; i < *(c->count); i++) {
        if (c->out[i].rule_id == rule_id) { found = (int)i; break; }
    }
    if (tombstoned) {
        if (found >= 0) {
            // Remove by shifting the tail down.
            for (uint16_t i = (uint16_t)found + 1; i < *(c->count); i++) {
                c->out[i - 1] = c->out[i];
            }
            (*(c->count))--;
        }
        return;
    }
    if (!slot) return;
    if (found >= 0) {
        c->out[found] = *slot;  // pending edit overrides NVS snapshot
    } else if (*(c->count) < c->max_count) {
        c->out[(*(c->count))++] = *slot;  // pending create not yet flushed
    }
}
}  // namespace

uint16_t rule_store_load_all(RuleSlot* out, uint16_t max_count) {
    if (!s_nvs) return 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    // Collect corrupt/stale keys during iteration and erase them after
    // the iterator is released. Erasing mid-iteration invalidates the
    // cursor in some NVS versions; defer to be safe.
    constexpr size_t kBadMax = 64;
    char bad_keys[kBadMax][16];
    size_t bad_n = 0;

    nvs_iterator_t it = nullptr;
    uint16_t count = 0;
    esp_err_t err = nvs_entry_find("nvs", NVS_NS, NVS_TYPE_BLOB, &it);
    while (err == ESP_OK && count < max_count) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        if (info.key[0] == 'r' && info.key[1] == '_') {
            size_t len = sizeof(RuleSlot);
            esp_err_t g = nvs_get_blob(s_nvs, info.key, &out[count], &len);
            const bool wrong_size = (g == ESP_OK) && (len != sizeof(RuleSlot));
            const bool bad_crc    = (g == ESP_OK) && !wrong_size &&
                                    (out[count].crc32 != rule_slot_crc(&out[count]));
            if (g == ESP_OK && !wrong_size && !bad_crc) {
                count++;
            } else if (g == ESP_OK) {
                ESP_LOGW(TAG, "%s key=%s — queued for erase",
                         wrong_size ? "size mismatch" : "CRC32 mismatch",
                         info.key);
                if (bad_n < kBadMax) {
                    memcpy(bad_keys[bad_n], info.key, sizeof(bad_keys[0]) - 1);
                    bad_keys[bad_n][sizeof(bad_keys[0]) - 1] = '\0';
                    bad_n++;
                }
            }
        }
        err = nvs_entry_next(&it);
    }
    if (it) nvs_release_iterator(it);

    // Best-effort cleanup — nullptr acc = per-op logging only; the
    // passthrough return still gates the per-key "erased" info line.
    for (size_t i = 0; i < bad_n; i++) {
        if (nvs_seq(nullptr, nvs_erase_key(s_nvs, bad_keys[i]),
                    TAG, "erase_key corrupt rule") == ESP_OK) {
            ESP_LOGI(TAG, "erased corrupt rule key=%s", bad_keys[i]);
        }
    }
    if (bad_n) nvs_seq(nullptr, nvs_commit(s_nvs), TAG, "commit corrupt-rule erase");
    xSemaphoreGive(s_mutex);

    // Merge writeback-overlay entries so readers see uncommitted edits.
    MergeCtx mctx{ out, &count, max_count };
    rule_store_foreach_dirty(merge_cb, &mctx);
    return count;
}

namespace {
struct MaxIdCtx { uint16_t max_id; };
// Overlay visitor: a pending WRITE (create/update not yet flushed) can hold
// an id higher than anything on flash; count it. TOMBSTONEs are ignored —
// the NVS key still exists until the flush lands, so the iterator below
// already accounts for the id; double-counting a deletion can't lower the
// max, and ignoring it can't reissue an id whose NVS row is still live.
void max_id_cb(uint16_t rule_id, bool tombstoned, const RuleSlot* slot, void* vctx) {
    (void)slot;
    if (tombstoned) return;
    auto* c = static_cast<MaxIdCtx*>(vctx);
    if (rule_id > c->max_id) c->max_id = rule_id;
}
}  // namespace

// F (P2-T18 def 1): highest rule_id across the ENTIRE persisted store
// (all NVS slots, up to ZAP_MAX_RULES) plus any uncommitted writeback
// edits — NOT just the subset cached in simple_rules' 64-entry array.
// simple_rules derives next_rule_id from this so a persisted-but-uncached
// rule's id can never be reissued (which would silently overwrite it).
// Cheap: parses the id straight out of the `r_%04X` key, no blob load.
uint16_t rule_store_max_id() {
    uint16_t max_id = 0;
    if (s_nvs) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        nvs_iterator_t it = nullptr;
        esp_err_t err = nvs_entry_find("nvs", NVS_NS, NVS_TYPE_BLOB, &it);
        while (err == ESP_OK) {
            nvs_entry_info_t info;
            nvs_entry_info(it, &info);
            if (info.key[0] == 'r' && info.key[1] == '_') {
                unsigned id = 0;
                if (sscanf(info.key + 2, "%4X", &id) == 1 && id <= 0xFFFF &&
                    (uint16_t)id > max_id) {
                    max_id = (uint16_t)id;
                }
            }
            err = nvs_entry_next(&it);
        }
        if (it) nvs_release_iterator(it);
        xSemaphoreGive(s_mutex);
    }
    // Fold in the writeback overlay (uncommitted creates/updates).
    MaxIdCtx mctx{ max_id };
    rule_store_foreach_dirty(max_id_cb, &mctx);
    return mctx.max_id;
}

// F (P2-T18 def 3): number of persisted rule slots in NVS (corrupt/stale
// keys still count — they're whatever `r_*` blobs exist). Cheap key-walk,
// no blob loads, no overlay merge (callers use it only to detect that the
// store holds more rules than a fixed-size cache can hold).
uint16_t rule_store_count() {
    if (!s_nvs) return 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    nvs_iterator_t it = nullptr;
    uint16_t n = 0;
    esp_err_t err = nvs_entry_find("nvs", NVS_NS, NVS_TYPE_BLOB, &it);
    while (err == ESP_OK) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        if (info.key[0] == 'r' && info.key[1] == '_') n++;
        err = nvs_entry_next(&it);
    }
    if (it) nvs_release_iterator(it);
    xSemaphoreGive(s_mutex);
    return n;
}

bool rule_store_set_enabled(uint16_t rule_id, bool enabled) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    RuleSlot slot{};
    if (!rule_store_load_unlocked(rule_id, &slot)) {
        xSemaphoreGive(s_mutex);
        return false;
    }
    slot.enabled = enabled ? 1 : 0;
    bool ok = rule_store_save_unlocked(&slot);
    xSemaphoreGive(s_mutex);
    return ok;
}
