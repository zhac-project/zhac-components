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

// Regression: a peer restart (its seq counter rewinds to 1) while we stay up
// must not wedge NEEDS_ACK delivery. A received SYNC clears the receive-side
// dedup high-water so the peer's restarted low seqs are accepted instead of
// being silently dropped as "behind the window". Without the SYNC reset the
// final frame stays dropped (on_frame_calls == 1) and device.list/get/set time
// out forever while NO_ACK heartbeats keep flowing.
TEST_CASE("session: SYNC resets rx dedup so peer-restart low seqs are accepted",
          "[session]") {
    Capture c;
    hap_session_init(make_cfg(c));

    // Establish a high high-water with one accepted NEEDS_ACK data frame.
    HapFrame hi{};
    hi.type  = HapMsgType::CMD;
    hi.seq   = 5000;
    hi.flags = HAP_FLAG_NEEDS_ACK;
    hap_session_on_receive(hi);
    TEST_ASSERT_EQUAL(1, c.on_frame_calls);

    // A fresh low seq now looks far behind the window → dropped (re-ACKed,
    // never dispatched). Documents the gate that the SYNC reset must defeat.
    HapFrame low{};
    low.type  = HapMsgType::CMD;
    low.seq   = 10;
    low.flags = HAP_FLAG_NEEDS_ACK;
    hap_session_on_receive(low);
    TEST_ASSERT_EQUAL(1, c.on_frame_calls);   // still dropped as stale-behind

    // Peer SYNC ⇒ new session ⇒ dedup state cleared.
    HapFrame sync{};
    sync.type = HapMsgType::SYNC;
    sync.seq  = 1;
    hap_session_on_receive(sync);

    // The same low seq is now accepted and dispatched.
    hap_session_on_receive(low);
    TEST_ASSERT_EQUAL(2, c.on_frame_calls);
}

// Regression: NO_ACK traffic must advance the receive high-water. Over a long
// continuous session the peer's seq races ahead on NO_ACK frames (heartbeats /
// attr updates) while NEEDS_ACK replies are rare; if the high-water only tracked
// NEEDS_ACK frames it would lag past 32768 and seq_diff would wrap, so a FRESH
// far-ahead reply gets misjudged "behind the window" and silently dropped — the
// 4h/10h device.list wedge with no reboot involved.
TEST_CASE("session: NO_ACK traffic advances high-water so a fresh far-ahead reply isn't dropped",
          "[session]") {
    Capture c;
    hap_session_init(make_cfg(c));

    // A NEEDS_ACK reply seeds the high-water at a low seq.
    HapFrame a{};
    a.type  = HapMsgType::CMD;
    a.seq   = 100;
    a.flags = HAP_FLAG_NEEDS_ACK;
    hap_session_on_receive(a);
    TEST_ASSERT_EQUAL(1, c.on_frame_calls);

    // NO_ACK traffic carries the peer's live seq far ahead (> half the uint16
    // space past the last NEEDS_ACK). It must drag the high-water with it.
    HapFrame hb{};
    hb.type  = HapMsgType::CMD;
    hb.seq   = 40000;
    hb.flags = HAP_FLAG_NO_ACK;
    hap_session_on_receive(hb);
    TEST_ASSERT_EQUAL(2, c.on_frame_calls);

    // A fresh NEEDS_ACK reply just ahead of the live seq must be accepted. With
    // a lagging high-water (seeded at 100) seq_diff(100,40001) wraps to a small
    // positive value >= the threshold and this would be dropped as stale.
    HapFrame r{};
    r.type  = HapMsgType::CMD;
    r.seq   = 40001;
    r.flags = HAP_FLAG_NEEDS_ACK;
    hap_session_on_receive(r);
    TEST_ASSERT_EQUAL(3, c.on_frame_calls);
}
