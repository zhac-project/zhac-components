// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "unity.h"
#include "zigbee_interview_utils.h"

TEST_CASE("basic probe order prefers endpoints with Basic cluster", "[zigbee_interview]") {
    ZapDevice dev{};
    dev.endpoint_count = 3;
    dev.endpoints[0] = 1;
    dev.endpoints[1] = 2;
    dev.endpoints[2] = 3;
    dev.clusters[1][0] = 0x0000;  // EP2 has Basic
    dev.clusters[2][0] = 0x0000;  // EP3 has Basic

    uint8_t order[8]{};
    uint8_t n = zigbee_interview_build_basic_probe_order(dev, order, 8);

    TEST_ASSERT_EQUAL_UINT8(3, n);
    TEST_ASSERT_EQUAL_UINT8(2, order[0]);
    TEST_ASSERT_EQUAL_UINT8(3, order[1]);
    TEST_ASSERT_EQUAL_UINT8(1, order[2]);
}

TEST_CASE("basic probe order preserves original order when no endpoint has Basic", "[zigbee_interview]") {
    ZapDevice dev{};
    dev.endpoint_count = 3;
    dev.endpoints[0] = 1;
    dev.endpoints[1] = 4;
    dev.endpoints[2] = 6;

    uint8_t order[8]{};
    uint8_t n = zigbee_interview_build_basic_probe_order(dev, order, 8);

    TEST_ASSERT_EQUAL_UINT8(3, n);
    TEST_ASSERT_EQUAL_UINT8(1, order[0]);
    TEST_ASSERT_EQUAL_UINT8(4, order[1]);
    TEST_ASSERT_EQUAL_UINT8(6, order[2]);
}

TEST_CASE("basic probe order removes duplicate endpoints", "[zigbee_interview]") {
    ZapDevice dev{};
    dev.endpoint_count = 4;
    dev.endpoints[0] = 1;
    dev.endpoints[1] = 2;
    dev.endpoints[2] = 2;
    dev.endpoints[3] = 3;
    dev.clusters[3][0] = 0x0000;  // EP3 has Basic

    uint8_t order[8]{};
    uint8_t n = zigbee_interview_build_basic_probe_order(dev, order, 8);

    TEST_ASSERT_EQUAL_UINT8(3, n);
    TEST_ASSERT_EQUAL_UINT8(3, order[0]);
    TEST_ASSERT_EQUAL_UINT8(1, order[1]);
    TEST_ASSERT_EQUAL_UINT8(2, order[2]);
}
