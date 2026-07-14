// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host tests for the esp_zigbee_backend pure RX mapper / TX builder (P0).
// No esp-zigbee-lib, no radio, no shims — the core is self-contained.
//   cmake -B build -S . && cmake --build build && ctest --test-dir build

#include "esp_zigbee_backend.h"

#include <cstdio>
#include <cstdint>

using namespace zhac::esp_zigbee;

static int g_failures = 0;
#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);\
            ++g_failures;                                                \
        }                                                                \
    } while (0)

// ── RX mapper ──────────────────────────────────────────────────────────────

static void test_map_unicast() {
    const uint8_t asdu[] = {0x18, 0x01, 0x0A, 0x00, 0x00, 0x10, 0x01};  // ZCL report
    EspZbApsInd in{};
    in.status       = 0;
    in.src_nwk      = 0x1234;
    in.src_endpoint = 1;
    in.cluster_id   = 0x0006;   // on/off
    in.profile_id   = kHaProfileId;
    in.dst_is_group = false;
    in.dst_group    = 0;
    in.lqi          = 200;
    in.asdu         = asdu;
    in.asdu_len     = sizeof(asdu);

    EspZbDecodeArgs out{};
    CHECK(esp_zb_map_incoming(in, out));
    CHECK(out.group_id == 0);            // unicast
    CHECK(out.cluster_id == 0x0006);
    CHECK(out.src_endpoint == 1);
    CHECK(out.linkquality == 200);
    CHECK(out.zcl == asdu);              // no copy — points at the ASDU
    CHECK(out.zcl_len == sizeof(asdu));
}

static void test_map_groupcast() {
    const uint8_t asdu[] = {0x01, 0x02, 0x00};
    EspZbApsInd in{};
    in.status = 0; in.src_nwk = 0x5678; in.src_endpoint = 3;
    in.cluster_id = 0x0008; in.profile_id = kHaProfileId;
    in.dst_is_group = true; in.dst_group = 0xABCD;   // groupcast
    in.lqi = 150; in.asdu = asdu; in.asdu_len = sizeof(asdu);

    EspZbDecodeArgs out{};
    CHECK(esp_zb_map_incoming(in, out));
    CHECK(out.group_id == 0xABCD);       // group address surfaces as group_id
    CHECK(out.cluster_id == 0x0008);
    CHECK(out.src_endpoint == 3);
}

static void test_map_rejects_bad_status() {
    const uint8_t asdu[] = {0x18, 0x00};
    EspZbApsInd in{};
    in.status = 0x82; in.asdu = asdu; in.asdu_len = sizeof(asdu);  // non-OK
    EspZbDecodeArgs out{};
    CHECK(!esp_zb_map_incoming(in, out));
}

static void test_map_rejects_empty_and_oversize() {
    EspZbDecodeArgs out{};
    // null ASDU
    EspZbApsInd n{}; n.status = 0; n.asdu = nullptr; n.asdu_len = 4;
    CHECK(!esp_zb_map_incoming(n, out));
    // zero length
    const uint8_t one[] = {0x18};
    EspZbApsInd z{}; z.status = 0; z.asdu = one; z.asdu_len = 0;
    CHECK(!esp_zb_map_incoming(z, out));
    // over the ASDU ceiling
    static uint8_t big[kAsduMax + 1] = {0};
    EspZbApsInd o{}; o.status = 0; o.asdu = big; o.asdu_len = sizeof(big);
    CHECK(!esp_zb_map_incoming(o, out));
    // exactly at the ceiling is accepted
    EspZbApsInd e{}; e.status = 0; e.src_endpoint = 1; e.cluster_id = 0x0000;
    e.asdu = big; e.asdu_len = kAsduMax;
    CHECK(esp_zb_map_incoming(e, out));
    CHECK(out.zcl_len == kAsduMax);
}

// ── TX builder ─────────────────────────────────────────────────────────────

static void test_build_unicast() {
    const uint8_t zcl[] = {0x01, 0x00, 0x02};  // ZCL default-response-ish
    EspZbApsReq out{};
    CHECK(esp_zb_build_outgoing(0x1234, 10, 0x0006, zcl, sizeof(zcl), out));
    CHECK(out.dst_nwk == 0x1234);
    CHECK(out.dst_endpoint == 10);
    CHECK(out.src_endpoint == kCoordEndpoint);
    CHECK(out.profile_id == kHaProfileId);
    CHECK(out.cluster_id == 0x0006);
    CHECK((out.tx_options & kTxOptAckReq) != 0);   // APS ACK requested
    CHECK(out.asdu == zcl);
    CHECK(out.asdu_len == sizeof(zcl));
}

static void test_build_rejects_empty_and_oversize() {
    EspZbApsReq out{};
    const uint8_t zcl[] = {0x01};
    CHECK(!esp_zb_build_outgoing(0x1, 1, 0x0006, nullptr, 3, out));  // null
    CHECK(!esp_zb_build_outgoing(0x1, 1, 0x0006, zcl, 0, out));      // empty
    static uint8_t big[kAsduMax + 1] = {0};
    CHECK(!esp_zb_build_outgoing(0x1, 1, 0x0006, big, sizeof(big), out));  // too big
    CHECK(esp_zb_build_outgoing(0x1, 1, 0x0006, big, kAsduMax, out));      // at ceiling ok
    CHECK(out.asdu_len == kAsduMax);
}

int main() {
    test_map_unicast();
    test_map_groupcast();
    test_map_rejects_bad_status();
    test_map_rejects_empty_and_oversize();
    test_build_unicast();
    test_build_rejects_empty_and_oversize();

    if (g_failures == 0) {
        std::printf("esp_zigbee_backend host: ALL PASS\n");
        return 0;
    }
    std::printf("esp_zigbee_backend host: %d FAILURE(S)\n", g_failures);
    return 1;
}
