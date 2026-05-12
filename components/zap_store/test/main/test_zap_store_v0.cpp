// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "unity.h"
#include "zap_store.h"
#include "zap_common.h"
#include "esp_rom_crc.h"
#include <cstring>

TEST_CASE("zap_store init does not crash", "[zap_store]") {
    zap_store_init();
    TEST_ASSERT_TRUE(zap_store_is_ready());
}

TEST_CASE("zap_store save and reload device", "[zap_store]") {
    zap_store_init();

    ZapDevice d{};
    d.ieee_addr  = 0xAABBCCDDEEFF0011ULL;
    d.nwk_addr   = 0x1234;
    d.endpoint_count = 1;
    d.endpoints[0]   = 1;
    snprintf(d.friendly_name, sizeof(d.friendly_name), "TestBulb");
    // crc32 is computed by zap_store_save_device — no need to set manually

    TEST_ASSERT_TRUE(zap_store_save_device(&d));

    ZapDevice pool[5]{};
    uint16_t n = zap_store_load_devices(pool, 5);
    TEST_ASSERT_GREATER_OR_EQUAL(1, n);

    bool found = false;
    for (uint16_t i = 0; i < n; i++) {
        if (pool[i].ieee_addr == d.ieee_addr) {
            TEST_ASSERT_EQUAL(d.nwk_addr, pool[i].nwk_addr);
            TEST_ASSERT_EQUAL_STRING("TestBulb", pool[i].friendly_name);
            found = true;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(found, "Saved device not found after reload");
}

TEST_CASE("zap_store rejects corrupted CRC", "[zap_store]") {
    zap_store_init();

    ZapDevice d{};
    d.ieee_addr  = 0x1122334455667788ULL;
    d.nwk_addr   = 0x5678;
    d.endpoint_count = 1;
    d.endpoints[0]   = 1;
    snprintf(d.friendly_name, sizeof(d.friendly_name), "GoodDevice");
    TEST_ASSERT_TRUE(zap_store_save_device(&d));

    // Corrupt the saved blob by writing with wrong CRC
    nvs_handle_t h;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_open("zap_v0", NVS_READWRITE, &h));
    ZapDevice tmp{};
    size_t sz = sizeof(ZapDevice);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_get_blob(h, "d0000", &tmp, &sz));
    tmp.nwk_addr = 0x9999;  // corrupt data but keep IEEE
    tmp.crc32 = 0xBADBAD00; // wrong CRC
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_blob(h, "d0000", &tmp, sizeof(tmp)));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_commit(h));
    nvs_close(h);

    // Reload — corrupted entry should be skipped
    ZapDevice pool[5]{};
    uint16_t n = zap_store_load_devices(pool, 5);
    // The corrupted slot is skipped, so we should not find nwk=0x9999
    for (uint16_t i = 0; i < n; i++) {
        TEST_ASSERT_NOT_EQUAL(0x9999, pool[i].nwk_addr);
    }
}

// Delete must not leave duplicate IEEE entries: save three, remove middle,
// reload — surviving entries must be unique and the victim must be gone.
TEST_CASE("zap_store delete preserves uniqueness", "[zap_store]") {
    zap_store_init();

    ZapDevice a{}, b{}, c{};
    a.ieee_addr = 0x1111111111111111ULL; a.nwk_addr = 0x0A01;
    b.ieee_addr = 0x2222222222222222ULL; b.nwk_addr = 0x0B02;
    c.ieee_addr = 0x3333333333333333ULL; c.nwk_addr = 0x0C03;
    TEST_ASSERT_TRUE(zap_store_save_device(&a));
    TEST_ASSERT_TRUE(zap_store_save_device(&b));
    TEST_ASSERT_TRUE(zap_store_save_device(&c));

    TEST_ASSERT_TRUE(zap_store_delete_device(b.ieee_addr));

    ZapDevice pool[8]{};
    uint16_t n = zap_store_load_devices(pool, 8);
    bool saw_a = false, saw_b = false, saw_c = false;
    for (uint16_t i = 0; i < n; i++) {
        if (pool[i].ieee_addr == a.ieee_addr) { TEST_ASSERT_FALSE(saw_a); saw_a = true; }
        if (pool[i].ieee_addr == b.ieee_addr) saw_b = true;
        if (pool[i].ieee_addr == c.ieee_addr) { TEST_ASSERT_FALSE(saw_c); saw_c = true; }
    }
    TEST_ASSERT_TRUE(saw_a);
    TEST_ASSERT_FALSE_MESSAGE(saw_b, "Deleted device reappeared after reload");
    TEST_ASSERT_TRUE(saw_c);
}
