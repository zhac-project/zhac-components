// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "unity.h"
#include "rule_store.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <cstring>

static void init_nvs() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
        nvs_flash_erase();
    nvs_flash_init();
    rule_store_init();
}

static RuleSlot make_slot(uint16_t id, const char* dsl) {
    RuleSlot s{};
    s.rule_id      = id;
    s.enabled      = 1;
    s.version      = 1;
    s.rule_type    = static_cast<uint8_t>(RuleType::SIMPLE);
    s.trigger_type = static_cast<uint8_t>(TriggerType::DEVICE_ATTR);
    s.src_len      = static_cast<uint16_t>(strlen(dsl));
    strncpy(reinterpret_cast<char*>(s.src), dsl, 499);
    return s;
}

TEST_CASE("rule_store: save and load round-trip", "[rule_store]") {
    init_nvs();
    RuleSlot slot = make_slot(0x0001, "ON kitchen#action=double DO zigbee.set socket state off ENDON");
    TEST_ASSERT_TRUE(rule_store_save(&slot));

    RuleSlot out{};
    TEST_ASSERT_TRUE(rule_store_load(0x0001, &out));
    TEST_ASSERT_EQUAL(0x0001, out.rule_id);
    TEST_ASSERT_EQUAL(1, out.enabled);
    TEST_ASSERT_EQUAL_STRING("ON kitchen#action=double DO zigbee.set socket state off ENDON",
                             reinterpret_cast<char*>(out.src));
}

TEST_CASE("rule_store: enabled flag persists", "[rule_store]") {
    init_nvs();
    RuleSlot slot = make_slot(0x0002, "ON System#Boot DO log boot ENDON");
    rule_store_save(&slot);

    // Round-trip the enabled flag through the overlay-aware load/save path
    // (rule_store_set_enabled was removed — callerless, bypassed the overlay).
    RuleSlot out{};
    TEST_ASSERT_TRUE(rule_store_load(0x0002, &out));
    out.enabled = 0;
    TEST_ASSERT_TRUE(rule_store_save(&out));
    TEST_ASSERT_TRUE(rule_store_load(0x0002, &out));
    TEST_ASSERT_EQUAL(0, out.enabled);

    out.enabled = 1;
    TEST_ASSERT_TRUE(rule_store_save(&out));
    TEST_ASSERT_TRUE(rule_store_load(0x0002, &out));
    TEST_ASSERT_EQUAL(1, out.enabled);
}

TEST_CASE("rule_store: delete removes from NVS", "[rule_store]") {
    init_nvs();
    RuleSlot slot = make_slot(0x0003, "ON temp#temperature>30 DO publish home/alert hot ENDON");
    rule_store_save(&slot);

    TEST_ASSERT_TRUE(rule_store_delete(0x0003));
    RuleSlot out{};
    TEST_ASSERT_FALSE(rule_store_load(0x0003, &out));
}

TEST_CASE("rule_store: load_all returns saved rules", "[rule_store]") {
    init_nvs();
    // Clear any leftover state
    RuleSlot all[256]{};
    uint16_t cnt = rule_store_load_all(all, 256);
    for (uint16_t i = 0; i < cnt; i++) rule_store_delete(all[i].rule_id);

    rule_store_save(&make_slot(0x0010, "ON a#b DO log x ENDON"));
    rule_store_save(&make_slot(0x0011, "ON c#d DO log y ENDON"));
    rule_store_save(&make_slot(0x0012, "ON e#f DO log z ENDON"));

    RuleSlot out[256]{};
    uint16_t n = rule_store_load_all(out, 256);
    TEST_ASSERT_EQUAL(3, n);
}

// Corrupt the stored CRC32 and verify rule_store_load refuses the record.
// This guards the integrity check added when RuleSlot.crc32 was introduced.
TEST_CASE("rule_store: CRC32 mismatch rejected on load", "[rule_store]") {
    init_nvs();
    RuleSlot slot = make_slot(0x0099, "ON x#y DO log z ENDON");
    TEST_ASSERT_TRUE(rule_store_save(&slot));

    // Tamper with the persisted blob: flip the stored CRC.
    nvs_handle_t h;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_open("zap_rules", NVS_READWRITE, &h));
    RuleSlot raw{};
    size_t sz = sizeof(RuleSlot);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_get_blob(h, "r_0099", &raw, &sz));
    raw.crc32 ^= 0xDEADBEEF;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_blob(h, "r_0099", &raw, sizeof(raw)));
    nvs_commit(h);
    nvs_close(h);

    RuleSlot out{};
    TEST_ASSERT_FALSE(rule_store_load(0x0099, &out));
}
