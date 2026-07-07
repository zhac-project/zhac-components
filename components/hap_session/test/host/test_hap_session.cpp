// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host characterization coverage for hap_session — the HAP SPI reliability
// layer that sits between the raw two-stage SPI transport and the application.
// Runs the REAL hap_session.cpp against esp/FreeRTOS shims + a controllable
// virtual clock (stubs/), so it exercises the actual sliding-window / ACK /
// dedup / SYNC logic with no ESP-IDF and no real time:
//   • init + send:   transport send cb fires; NEEDS_ACK frames take a window
//                    slot; next_seq() is monotonic-from-1 and never returns 0
//                    (incl. the uint16 wrap that skips 0); seq==0 is rejected;
//                    NO_ACK / SYNC bypass the window.
//   • receive:       a NEEDS_ACK data frame dispatches once and is ACKed, with
//                    the ACK echoing the request seq in ack_seq.
//   • dedup:         the same (seq,type) delivered twice dispatches once but is
//                    re-ACKed both times (peer is retransmitting a lost ACK).
//   • retransmit:    an un-ACKed frame is re-sent once per ACK_TIMEOUT, up to
//                    MAX_RETRIES; one further timeout trips on_link_dead.
//   • ACK clears:    delivering the matching ACK frees the slot — no retransmit.
//   • reset_link:    abandons in-flight slots (no retransmit afterwards) WITHOUT
//                    emitting a SYNC and WITHOUT restarting the seq counter.
//   • SYNC:          a received SYNC fires on_sync and resets receive-side dedup
//                    so a previously-seen seq is accepted again.
//   • high-water:    NO_ACK traffic advances the monotonic high-water (the
//                    wrap-fix), and the stale check is uint16-wrap-aware across
//                    the 0xFFFF->0 boundary.
//   • window-full:   the WIN_SIZE+1-th in-flight NEEDS_ACK send is refused;
//                    NO_ACK still bypasses; an ACK frees a slot for a refit.
//
// NOTE (characterization, not aspiration): the task brief described
// hap_session_reset_link() as "emits a SYNC and restarts next_seq". The code
// does NEITHER — the header explicitly documents that it preserves the outbound
// seq counter (peer expects continuity) and only frees the window. The SYNC is
// emitted by the higher transport layer, not here. The tests below assert the
// ACTUAL behavior.
#include "hap_session.h"
#include "hap_protocol.h"
#include "esp_timer.h"   // stub_clock_advance_ms / stub_clock_reset

#include <cstdio>
#include <cstdint>
#include <vector>

// ── Constants mirrored from hap_session.cpp (they are file-static there) ──────
static constexpr uint32_t ACK_TIMEOUT_MS = 1000;
static constexpr int      MAX_RETRIES    = 5;
static constexpr int      WIN_SIZE       = 16;

// ── Failure bookkeeping + assertion macro (repo house style) ─────────────────
static int s_failures = 0;
#define CHECK(cond, msg) do {                                         \
    if (cond) { printf("PASS: %s\n", msg); }                          \
    else      { printf("FAIL: %s\n", msg); s_failures++; }            \
} while (0)

// ── Mock transport / callback recorders ──────────────────────────────────────
struct SentRec {
    HapMsgType type;
    uint16_t   seq;
    uint16_t   ack_seq;
    uint8_t    flags;
    uint16_t   payload_len;
};

static std::vector<SentRec> g_sent;
static int      g_on_frame_count   = 0;
static uint16_t g_on_frame_last_seq = 0;
static int      g_on_sync_count    = 0;
static int      g_link_dead_count  = 0;

static void reset_recorders() {
    g_sent.clear();
    g_on_frame_count    = 0;
    g_on_frame_last_seq = 0;
    g_on_sync_count     = 0;
    g_link_dead_count   = 0;
}

static HapSessionCfg make_cfg() {
    HapSessionCfg c;
    c.send = [](const HapFrame& f) {
        g_sent.push_back({f.type, f.seq, f.ack_seq, f.flags, f.payload_len});
    };
    c.on_frame = [](const HapFrame& f) {
        g_on_frame_count++;
        g_on_frame_last_seq = f.seq;
    };
    c.on_sync      = [](const HapFrame&) { g_on_sync_count++; };
    c.on_link_dead = []()               { g_link_dead_count++; };
    return c;
}

// Build a bare application frame (no payload).
static HapFrame mk(HapMsgType type, uint16_t seq, uint8_t flags) {
    HapFrame f{};
    f.type        = type;
    f.seq         = seq;
    f.ack_seq     = 0;
    f.flags       = flags;
    f.payload     = nullptr;
    f.payload_len = 0;
    return f;
}

// Build an ACK frame that correlates to `req_seq` (as hap_make_reply would).
static HapFrame mk_ack(uint16_t own_seq, uint16_t req_seq) {
    HapFrame a = mk(HapMsgType::ACK, own_seq, HAP_FLAG_NO_ACK);
    a.ack_seq  = req_seq;
    return a;
}

int main() {
    // ── Group 0: pre-init guards (NO init yet) ───────────────────────────────
    CHECK(hap_session_next_seq() == 0,
          "next_seq() before init returns the uninit sentinel (0)");
    {
        HapFrame f = mk(HapMsgType::CMD, 1, HAP_FLAG_NEEDS_ACK);
        CHECK(!hap_session_send(f),
              "send() before init returns false (no transport configured)");
    }

    HapSessionCfg cfg = make_cfg();

    // ── Group 1: init + send basics ──────────────────────────────────────────
    hap_session_init(cfg);
    reset_recorders();
    stub_clock_reset();

    uint16_t s1 = hap_session_next_seq();
    uint16_t s2 = hap_session_next_seq();
    uint16_t s3 = hap_session_next_seq();
    CHECK(s1 == 1 && s2 == 2 && s3 == 3, "next_seq() is monotonic starting at 1");
    CHECK(s1 != 0 && s2 != 0 && s3 != 0, "next_seq() never returns 0");

    {
        HapFrame f = mk(HapMsgType::CMD, hap_session_next_seq(), HAP_FLAG_NEEDS_ACK);
        CHECK(hap_session_send(f), "send() of a NEEDS_ACK frame returns true");
        CHECK(g_sent.size() == 1, "transport send cb invoked once for the NEEDS_ACK frame");
        CHECK(g_sent[0].seq == f.seq && (g_sent[0].flags & HAP_FLAG_NEEDS_ACK),
              "the on-wire frame carries the caller's fresh seq + NEEDS_ACK flag");
    }
    {
        reset_recorders();
        HapFrame n = mk(HapMsgType::HEARTBEAT, hap_session_next_seq(), HAP_FLAG_NO_ACK);
        CHECK(hap_session_send(n), "send() of a NO_ACK frame returns true");
        CHECK(g_sent.size() == 1 && g_sent[0].seq == n.seq,
              "NO_ACK frame goes straight to the wire (window bypass)");
    }
    {
        reset_recorders();
        HapFrame sy = mk(HapMsgType::SYNC, hap_session_next_seq(), 0);
        CHECK(hap_session_send(sy), "send() of a SYNC frame returns true");
        CHECK(g_sent.size() == 1, "SYNC frame goes straight to the wire (window bypass)");
    }
    {
        reset_recorders();
        HapFrame z = mk(HapMsgType::CMD, 0, HAP_FLAG_NEEDS_ACK);
        CHECK(!hap_session_send(z), "send() with seq==0 (uninit sentinel) is rejected");
        CHECK(g_sent.empty(), "the rejected seq==0 frame never reaches the wire");
    }

    // ── Group 2: next_seq() skips 0 across the uint16 wrap ───────────────────
    hap_session_init(cfg);
    {
        bool never_zero = true;
        bool wrapped_to_one = false;
        uint16_t prev = 0;
        for (long i = 0; i < 70000; i++) {
            uint16_t v = hap_session_next_seq();
            if (v == 0) never_zero = false;
            if (prev == 65535 && v == 1) wrapped_to_one = true;
            prev = v;
        }
        CHECK(never_zero, "next_seq() never yields 0 across a full uint16 wrap (70000 calls)");
        CHECK(wrapped_to_one, "next_seq() rolls 65535 -> 1 (skips 0 at the wrap)");
    }

    // ── Group 3: receive NEEDS_ACK -> dispatch once + ACK echo ───────────────
    hap_session_init(cfg);
    reset_recorders();
    {
        HapFrame rx = mk(HapMsgType::CMD, 42, HAP_FLAG_NEEDS_ACK);
        hap_session_on_receive(rx);
        CHECK(g_on_frame_count == 1, "on_frame fires once for a NEEDS_ACK data frame");
        CHECK(g_on_frame_last_seq == 42, "on_frame receives the frame with its original seq");
        CHECK(g_sent.size() == 1, "exactly one ACK frame is emitted in response");
        CHECK(g_sent[0].type == HapMsgType::ACK, "the emitted frame is an ACK");
        CHECK(g_sent[0].ack_seq == 42, "the ACK echoes the received seq in ack_seq");
        CHECK((g_sent[0].flags & HAP_FLAG_NO_ACK), "the ACK is itself NO_ACK (not window-tracked)");
        CHECK(g_sent[0].seq != 0, "the ACK carries a fresh nonzero seq");
    }

    // ── Group 4: dedup — same (seq,type) twice dispatches once, re-ACKs both ─
    hap_session_init(cfg);
    reset_recorders();
    {
        HapFrame rx = mk(HapMsgType::CMD, 77, HAP_FLAG_NEEDS_ACK);
        hap_session_on_receive(rx);
        CHECK(g_on_frame_count == 1, "first delivery dispatches to on_frame");
        CHECK(g_sent.size() == 1 && g_sent[0].ack_seq == 77, "first delivery is ACKed (ack_seq=77)");

        hap_session_on_receive(rx);   // exact duplicate (lost-ACK retransmit)
        CHECK(g_on_frame_count == 1, "duplicate NEEDS_ACK is NOT re-dispatched (dedup)");
        CHECK(g_sent.size() == 2, "duplicate is still re-ACKed (peer lost the first ACK)");
        CHECK(g_sent[1].type == HapMsgType::ACK && g_sent[1].ack_seq == 77,
              "the re-ACK again echoes the same seq");
    }

    // ── Group 5: retransmit ladder -> on_link_dead ───────────────────────────
    hap_session_init(cfg);
    reset_recorders();
    stub_clock_reset();
    {
        HapFrame f = mk(HapMsgType::DEVICE_LIST, hap_session_next_seq(), HAP_FLAG_NEEDS_ACK);
        CHECK(hap_session_send(f), "send a NEEDS_ACK frame to arm the retransmit window");
        CHECK(g_sent.size() == 1, "initial send recorded");

        hap_session_tick();   // before timeout
        CHECK(g_sent.size() == 1, "tick() before ACK_TIMEOUT does not retransmit");

        bool each_retx_once = true;
        for (int i = 1; i <= MAX_RETRIES; i++) {
            stub_clock_advance_ms(ACK_TIMEOUT_MS + 50);
            size_t before = g_sent.size();
            hap_session_tick();
            if (g_sent.size() != before + 1) each_retx_once = false;
        }
        CHECK(each_retx_once, "each elapsed ACK_TIMEOUT retransmits the frame exactly once");
        CHECK(g_sent.size() == (size_t)(1 + MAX_RETRIES),
              "total sends == initial + MAX_RETRIES retransmits");
        CHECK(g_link_dead_count == 0, "on_link_dead has NOT fired while retries remain");

        stub_clock_advance_ms(ACK_TIMEOUT_MS + 50);
        size_t before_dead = g_sent.size();
        hap_session_tick();   // MAX_RETRIES+1-th timeout
        CHECK(g_link_dead_count == 1, "on_link_dead fires after MAX_RETRIES failed retransmits");
        CHECK(g_sent.size() == before_dead, "the link-dead tick sends nothing (slot abandoned)");

        stub_clock_advance_ms(ACK_TIMEOUT_MS + 50);
        hap_session_tick();
        CHECK(g_link_dead_count == 1 && g_sent.size() == before_dead,
              "a dead slot is inert on subsequent ticks (no re-fire, no re-send)");
    }

    // ── Group 6: a matching ACK clears the window (no retransmit) ────────────
    hap_session_init(cfg);
    reset_recorders();
    stub_clock_reset();
    {
        HapFrame f = mk(HapMsgType::DEVICE_INFO, hap_session_next_seq(), HAP_FLAG_NEEDS_ACK);
        hap_session_send(f);
        CHECK(g_sent.size() == 1, "send arms a window slot");

        HapFrame ack = mk_ack(/*own*/ 9000, /*req*/ f.seq);
        hap_session_on_receive(ack);
        CHECK(g_on_frame_count == 0, "an ACK is not dispatched to on_frame");

        stub_clock_advance_ms(ACK_TIMEOUT_MS + 50);
        hap_session_tick();
        CHECK(g_sent.size() == 1, "the ACK cleared the slot — tick() does not retransmit");
    }

    // ── Group 7: reset_link abandons the window, keeps seq, sends no SYNC ─────
    hap_session_init(cfg);
    reset_recorders();
    stub_clock_reset();
    {
        uint16_t seq_a = hap_session_next_seq();
        HapFrame f = mk(HapMsgType::DEVICE_LIST, seq_a, HAP_FLAG_NEEDS_ACK);
        hap_session_send(f);
        CHECK(g_sent.size() == 1, "a NEEDS_ACK frame is in flight before reset_link()");

        hap_session_reset_link();
        CHECK(g_sent.size() == 1,
              "reset_link() puts NO frame on the wire (does not emit a SYNC itself)");

        stub_clock_advance_ms(ACK_TIMEOUT_MS + 50);
        hap_session_tick();
        CHECK(g_sent.size() == 1, "reset_link() freed the in-flight slot — no retransmit");

        uint16_t seq_b = hap_session_next_seq();
        CHECK(seq_b == (uint16_t)(seq_a + 1),
              "reset_link() preserves the outbound seq counter (continuity, no restart)");
    }

    // ── Group 8: received SYNC fires on_sync and resets receive-side dedup ────
    hap_session_init(cfg);
    reset_recorders();
    {
        HapFrame rx = mk(HapMsgType::CMD, 555, HAP_FLAG_NEEDS_ACK);
        hap_session_on_receive(rx);
        hap_session_on_receive(rx);
        CHECK(g_on_frame_count == 1, "seq=555 dispatched once, its duplicate deduped");

        reset_recorders();
        HapFrame sync = mk(HapMsgType::SYNC, 1, 0);
        hap_session_on_receive(sync);
        CHECK(g_on_sync_count == 1, "a received SYNC fires on_sync");
        CHECK(g_on_frame_count == 0, "SYNC itself does not dispatch to on_frame");

        hap_session_on_receive(rx);   // seq=555 again, post-SYNC
        CHECK(g_on_frame_count == 1,
              "SYNC reset the dedup state — the previously-seen seq is accepted again");
    }

    // ── Group 9a: NO_ACK traffic advances the monotonic high-water ───────────
    // (the wrap-fix: without advancing on NO_ACK frames the high-water would lag
    //  the peer's live seq and eventually mis-drop fresh replies.)
    hap_session_init(cfg);
    reset_recorders();
    {
        hap_session_on_receive(mk(HapMsgType::CMD, 100, HAP_FLAG_NEEDS_ACK));
        CHECK(g_on_frame_count == 1, "NEEDS_ACK seq=100 accepted (sets high-water=100)");

        reset_recorders();
        hap_session_on_receive(mk(HapMsgType::DEVICE_EVENT, 200, HAP_FLAG_NO_ACK));
        CHECK(g_on_frame_count == 1, "NO_ACK seq=200 is dispatched (not deduped) ...");

        reset_recorders();
        hap_session_on_receive(mk(HapMsgType::CMD, 150, HAP_FLAG_NEEDS_ACK));
        CHECK(g_on_frame_count == 0,
              "... and it advanced the high-water: NEEDS_ACK seq=150 is now dropped as stale");
        CHECK(g_sent.size() == 1 && g_sent[0].type == HapMsgType::ACK && g_sent[0].ack_seq == 150,
              "the stale-dropped duplicate is still re-ACKed");
    }

    // ── Group 9b: the stale check is uint16-wrap-aware across 0xFFFF->0 ───────
    hap_session_init(cfg);
    reset_recorders();
    {
        // Accept a frame near the top of the seq space, then walk the high-water
        // across the wrap with NO_ACK frames (each < half-space ahead so it
        // advances forward). None land on 0 (the note_peer_seq sentinel).
        hap_session_on_receive(mk(HapMsgType::CMD, 0xFFF0, HAP_FLAG_NEEDS_ACK));
        const uint16_t walk[] = {0xFFFA, 0x0004, 0x000E, 0x0018};   // steps of 0x0A, crosses 0
        for (uint16_t s : walk) {
            hap_session_on_receive(mk(HapMsgType::DEVICE_EVENT, s, HAP_FLAG_NO_ACK));
        }
        // high-water is now 0x0018 having wrapped past 0xFFFF.
        reset_recorders();
        hap_session_on_receive(mk(HapMsgType::CMD, 0x0019, HAP_FLAG_NEEDS_ACK));
        CHECK(g_on_frame_count == 1,
              "post-wrap: a fresh frame just ahead of the wrapped high-water is accepted");

        reset_recorders();
        hap_session_on_receive(mk(HapMsgType::CMD, 0xFFD0, HAP_FLAG_NEEDS_ACK));
        CHECK(g_on_frame_count == 0,
              "post-wrap: a frame wrap-distance >= threshold behind is dropped as stale");
    }

    // ── Group 10: sliding-window capacity (WIN_SIZE) ─────────────────────────
    hap_session_init(cfg);
    reset_recorders();
    stub_clock_reset();
    {
        bool all_ok = true;
        for (int i = 0; i < WIN_SIZE; i++) {
            HapFrame f = mk(HapMsgType::CMD, hap_session_next_seq(), HAP_FLAG_NEEDS_ACK);
            if (!hap_session_send(f)) all_ok = false;
        }
        CHECK(all_ok, "all WIN_SIZE NEEDS_ACK frames are admitted to the window");
        CHECK(g_sent.size() == (size_t)WIN_SIZE, "each admitted frame reached the wire");

        HapFrame overflow = mk(HapMsgType::CMD, hap_session_next_seq(), HAP_FLAG_NEEDS_ACK);
        CHECK(!hap_session_send(overflow),
              "the WIN_SIZE+1-th in-flight NEEDS_ACK send is refused (window full)");
        CHECK(g_sent.size() == (size_t)WIN_SIZE, "the window-full frame is not put on the wire");

        HapFrame na = mk(HapMsgType::HEARTBEAT, hap_session_next_seq(), HAP_FLAG_NO_ACK);
        CHECK(hap_session_send(na), "a NO_ACK frame still sends when the ACK window is full");

        // ACK the first in-flight frame (seq==1 here) -> frees a slot -> refit.
        hap_session_on_receive(mk_ack(/*own*/ 41000, /*req*/ 1));
        HapFrame refit = mk(HapMsgType::CMD, hap_session_next_seq(), HAP_FLAG_NEEDS_ACK);
        CHECK(hap_session_send(refit),
              "after an ACK frees a slot, a new NEEDS_ACK send is admitted again");
    }

    printf(s_failures ? "\nFAILED (%d)\n" : "\nOK\n", s_failures);
    return s_failures ? 1 : 0;
}
