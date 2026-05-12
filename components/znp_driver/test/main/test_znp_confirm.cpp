// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "unity.h"
#include "znp_confirm.h"
#include "znp_transport.h"
#include "znp_types.h"

static ZnpFrame make_confirm(uint8_t status, uint8_t endpoint,
                             uint8_t trans_id) {
    ZnpFrame f{};
    f.cmd0 = 0x44; f.cmd1 = 0x80; f.len = 3;
    f.data[0] = status; f.data[1] = endpoint; f.data[2] = trans_id;
    return f;
}

TEST_CASE("confirm — basic match signals status", "[znp_confirm]") {
    int s = znp_confirm_reserve(0x42);
    TEST_ASSERT_NOT_EQUAL(-1, s);
    ZnpFrame f = make_confirm(0x00, 0x01, 0x42);
    znp_areq_dispatch(f);
    TEST_ASSERT_EQUAL(0x00, znp_confirm_wait(s, 100));
}

TEST_CASE("confirm — MAC-expired status propagates", "[znp_confirm]") {
    int s = znp_confirm_reserve(0x55);
    TEST_ASSERT_NOT_EQUAL(-1, s);
    ZnpFrame f = make_confirm(0xF0, 0x01, 0x55);
    znp_areq_dispatch(f);
    TEST_ASSERT_EQUAL(0xF0, znp_confirm_wait(s, 100));
}

TEST_CASE("confirm — wrong trans_id times out", "[znp_confirm]") {
    int s = znp_confirm_reserve(0x42);
    TEST_ASSERT_NOT_EQUAL(-1, s);
    ZnpFrame f = make_confirm(0x00, 0x01, 0xAA);
    znp_areq_dispatch(f);
    TEST_ASSERT_EQUAL(-1, znp_confirm_wait(s, 20));
}

TEST_CASE("confirm — ring fills at 16 slots", "[znp_confirm]") {
    int h[16];
    for (int i = 0; i < 16; i++) {
        h[i] = znp_confirm_reserve(static_cast<uint8_t>(i));
        TEST_ASSERT_NOT_EQUAL(-1, h[i]);
    }
    TEST_ASSERT_EQUAL(-1, znp_confirm_reserve(0x99));
    for (int i = 0; i < 16; i++) znp_confirm_release(h[i]);
}

TEST_CASE("confirm — release makes slot reusable", "[znp_confirm]") {
    int a = znp_confirm_reserve(0x11);
    TEST_ASSERT_NOT_EQUAL(-1, a);
    znp_confirm_release(a);
    int b = znp_confirm_reserve(0x22);
    TEST_ASSERT_NOT_EQUAL(-1, b);
    znp_confirm_release(b);
}

TEST_CASE("confirm — truncated AREQ (len<3) ignored", "[znp_confirm]") {
    int s = znp_confirm_reserve(0x01);
    TEST_ASSERT_NOT_EQUAL(-1, s);
    ZnpFrame f{};
    f.cmd0 = 0x44; f.cmd1 = 0x80; f.len = 2;
    f.data[0] = 0x00; f.data[1] = 0x01;
    znp_areq_dispatch(f);
    TEST_ASSERT_EQUAL(-1, znp_confirm_wait(s, 20));
}
