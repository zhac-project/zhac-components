// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: Apache-2.0
#include "unity.h"
#include "hap_session.h"
#include "hap_protocol.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <vector>

namespace {
struct Capture {
    std::vector<HapFrame> sent;
    int                   on_frame_calls   = 0;
    int                   link_dead_calls  = 0;
    HapFrame              last_app{};
};
static Capture* g_cap = nullptr;

HapSessionCfg make_cfg(Capture& c) {
    g_cap = &c;
    HapSessionCfg cfg{};
    cfg.send         = [](const HapFrame& f) { g_cap->sent.push_back(f); };
    cfg.on_frame     = [](const HapFrame& f) {
        g_cap->on_frame_calls++; g_cap->last_app = f;
    };
    cfg.on_sync      = [](const HapFrame&) {};
    cfg.on_link_dead = [] { g_cap->link_dead_calls++; };
    return cfg;
}
} // anon

TEST_CASE("session: NEEDS_ACK occupies slot until ACK", "[session]") {
    Capture c;
    hap_session_init(make_cfg(c));
    HapFrame f{};
    f.type  = HapMsgType::CMD;
    f.seq   = hap_session_next_seq();
    f.flags = HAP_FLAG_NEEDS_ACK;
    TEST_ASSERT_TRUE(hap_session_send(f));
    TEST_ASSERT_EQUAL(1u, c.sent.size());

    HapFrame ack{};
    ack.type    = HapMsgType::ACK;
    ack.ack_seq = f.seq;
    hap_session_on_receive(ack);

    for (int i = 0; i < 8; i++) {
        HapFrame f2{};
        f2.type  = HapMsgType::CMD;
        f2.seq   = hap_session_next_seq();
        f2.flags = HAP_FLAG_NEEDS_ACK;
        TEST_ASSERT_TRUE(hap_session_send(f2));
    }
}

TEST_CASE("session: NO_ACK bypasses window", "[session]") {
    Capture c;
    hap_session_init(make_cfg(c));
    for (int i = 0; i < 32; i++) {
        HapFrame f{};
        f.type  = HapMsgType::HEARTBEAT;
        f.seq   = hap_session_next_seq();
        f.flags = HAP_FLAG_NO_ACK;
        TEST_ASSERT_TRUE(hap_session_send(f));
    }
    TEST_ASSERT_EQUAL(32u, c.sent.size());
}

TEST_CASE("session: 9th NEEDS_ACK while 8 outstanding returns false", "[session]") {
    Capture c;
    hap_session_init(make_cfg(c));
    for (int i = 0; i < 8; i++) {
        HapFrame f{};
        f.type  = HapMsgType::CMD;
        f.seq   = hap_session_next_seq();
        f.flags = HAP_FLAG_NEEDS_ACK;
        TEST_ASSERT_TRUE(hap_session_send(f));
    }
    HapFrame ninth{};
    ninth.type  = HapMsgType::CMD;
    ninth.seq   = hap_session_next_seq();
    ninth.flags = HAP_FLAG_NEEDS_ACK;
    TEST_ASSERT_FALSE(hap_session_send(ninth));
}

TEST_CASE("session: stale ACK with wrong ack_seq is ignored", "[session]") {
    Capture c;
    hap_session_init(make_cfg(c));
    HapFrame f{};
    f.type  = HapMsgType::CMD;
    f.seq   = hap_session_next_seq();
    f.flags = HAP_FLAG_NEEDS_ACK;
    hap_session_send(f);

    HapFrame bogus{};
    bogus.type    = HapMsgType::ACK;
    bogus.ack_seq = (uint16_t)(f.seq + 999);
    hap_session_on_receive(bogus);

    for (int i = 0; i < 7; i++) {
        HapFrame x{};
        x.type  = HapMsgType::CMD;
        x.seq   = hap_session_next_seq();
        x.flags = HAP_FLAG_NEEDS_ACK;
        TEST_ASSERT_TRUE(hap_session_send(x));
    }
    HapFrame last{};
    last.type  = HapMsgType::CMD;
    last.seq   = hap_session_next_seq();
    last.flags = HAP_FLAG_NEEDS_ACK;
    TEST_ASSERT_FALSE(hap_session_send(last));
}

TEST_CASE("session: 3 retx without ACK fires on_link_dead", "[session]") {
    Capture c;
    hap_session_init(make_cfg(c));
    HapFrame f{};
    f.type  = HapMsgType::CMD;
    f.seq   = hap_session_next_seq();
    f.flags = HAP_FLAG_NEEDS_ACK;
    hap_session_send(f);

    for (int i = 0; i < 8; i++) {
        vTaskDelay(pdMS_TO_TICKS(120));
        hap_session_tick();
    }
    TEST_ASSERT_EQUAL(1, c.link_dead_calls);
    TEST_ASSERT_GREATER_THAN_INT(3, (int)c.sent.size());
}

TEST_CASE("session: duplicate NEEDS_ACK suppressed; ACK still sent", "[session]") {
    Capture c;
    hap_session_init(make_cfg(c));

    HapFrame in{};
    in.type    = HapMsgType::CMD;
    in.seq     = 100;
    in.flags   = HAP_FLAG_NEEDS_ACK;
    hap_session_on_receive(in);
    hap_session_on_receive(in);

    TEST_ASSERT_EQUAL(1, c.on_frame_calls);
    TEST_ASSERT_EQUAL(2u, c.sent.size());
    TEST_ASSERT_EQUAL(HapMsgType::ACK, c.sent[0].type);
    TEST_ASSERT_EQUAL(HapMsgType::ACK, c.sent[1].type);
}
