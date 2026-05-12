// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// components/hap_json/test/main/test_hap_json.cpp
#include "unity.h"
#include "hap_json.h"
#include <cstring>
#include <cstdio>

TEST_CASE("hap_json: encode/decode heartbeat round-trip", "[hap_json]") {
    uint8_t buf[128];
    uint16_t len = 0;
    TEST_ASSERT_TRUE(hap_json_encode_heartbeat(buf, sizeof(buf), &len, 3600, 180000));
    TEST_ASSERT_GREATER_THAN(0, len);

    uint32_t uptime = 0;
    TEST_ASSERT_TRUE(hap_json_decode_heartbeat(buf, len, uptime));
    TEST_ASSERT_EQUAL(3600, uptime);
}

TEST_CASE("hap_json: encode/decode SYNC_REQ round-trip", "[hap_json]") {
    uint8_t buf[128];
    uint16_t len = 0;
    TEST_ASSERT_TRUE(hap_json_encode_sync_req(buf, sizeof(buf), &len, 0xDEADBEEF, "0.3.1"));
    TEST_ASSERT_GREATER_THAN(0, len);

    HapSyncInfo info{};
    TEST_ASSERT_TRUE(hap_json_decode_sync(buf, len, info));
    TEST_ASSERT_EQUAL(0xDEADBEEFu, info.session_id);
    TEST_ASSERT_EQUAL_STRING("0.3.1", info.fw_ver);
}

TEST_CASE("hap_json: encode SYNC_ACK contains device_count", "[hap_json]") {
    uint8_t buf[128];
    uint16_t len = 0;
    TEST_ASSERT_TRUE(hap_json_encode_sync_ack(buf, sizeof(buf), &len, 0x12345678, "0.4.0", 42));

    HapSyncInfo info{};
    TEST_ASSERT_TRUE(hap_json_decode_sync(buf, len, info));
    TEST_ASSERT_EQUAL(42, info.device_count);
    TEST_ASSERT_EQUAL(0x12345678u, info.session_id);
}

TEST_CASE("hap_json: decode SET_ATTRIBUTE", "[hap_json]") {
    // Wire format: {"ieee":"0xAABBCCDDEEFF0011","ep":1,"cl":6,"at":0,"val":1}
    const char* json = "{\"ieee\":\"0xAABBCCDDEEFF0011\",\"ep\":1,\"cl\":6,\"at\":0,\"val\":1}";
    HapSetAttrReq req{};
    TEST_ASSERT_TRUE(hap_json_decode_set_attr(
        reinterpret_cast<const uint8_t*>(json), static_cast<uint16_t>(strlen(json)), req));
    TEST_ASSERT_EQUAL(0xAABBCCDDEEFF0011ULL, req.ieee);
    TEST_ASSERT_EQUAL(1, req.ep);
    TEST_ASSERT_EQUAL(6, req.cluster);
    TEST_ASSERT_EQUAL(0, req.attr);
    TEST_ASSERT_EQUAL(1, req.val);
}

TEST_CASE("hap_json: encode DEVICE_LIST 2 devices", "[hap_json]") {
    ZapDevice devs[2]{};
    devs[0].ieee_addr = 0xAABBCCDDEEFF0011ULL;
    devs[0].nwk_addr  = 0x1234;
    snprintf(devs[0].friendly_name, sizeof(devs[0].friendly_name), "Bulb1");
    devs[1].ieee_addr = 0x1122334455667788ULL;
    devs[1].nwk_addr  = 0x5678;
    snprintf(devs[1].friendly_name, sizeof(devs[1].friendly_name), "Sensor1");
    devs[0].endpoint_count = 2;
    devs[1].endpoint_count = 1;

    uint8_t buf[512];
    uint16_t len = 0;
    TEST_ASSERT_TRUE(hap_json_encode_device_list(buf, sizeof(buf), &len, devs, 2));
    TEST_ASSERT_GREATER_THAN(0, len);
    // Should contain both IEEE addresses
    char ieee0[20]; snprintf(ieee0, sizeof(ieee0), "0xAABBCCDDEEFF0011");
    TEST_ASSERT_NOT_NULL(strstr(reinterpret_cast<char*>(buf), ieee0));
}

TEST_CASE("hap_json: encode BULK_STATE_UPDATE 2 events", "[hap_json]") {
    HapDeviceEvent evs[2]{};
    evs[0].ieee = 0xAABBCCDDEEFF0011ULL; evs[0].cluster = 6;  evs[0].attr = 0; evs[0].val = 1;
    evs[1].ieee = 0x1122334455667788ULL; evs[1].cluster = 0x0402; evs[1].attr = 0; evs[1].val = 2350;

    uint8_t buf[512];
    uint16_t len = 0;
    TEST_ASSERT_TRUE(hap_json_encode_bulk(buf, sizeof(buf), &len, evs, 2));
    TEST_ASSERT_GREATER_THAN(0, len);
    TEST_ASSERT_NOT_NULL(strstr(reinterpret_cast<char*>(buf), "devs"));
}
