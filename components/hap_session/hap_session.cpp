// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// components/hap_session/hap_session.cpp
#include "hap_session.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "metrics/metrics_macros.h"
#include <cstring>
#include <cstdint>

static const char* TAG = "hap_session";

static constexpr uint8_t  WIN_SIZE       = 16;
// 1000 ms × 5 retries = 5000 ms link-dead budget. Real-world ACKs for
// kilobyte-class responses (DEVICE_LIST, DEVICE_INFO) routinely land
// 800-1000 ms after send when the receiver is mid-WS-broadcast or
// JSON-encoding a large body. With timeout=500 ms the first retx kept
// firing on healthy links — wasting bandwidth and producing noisy
// duplicate ACKs from each retx the peer dutifully answered. 1000 ms
// covers the slow path; 5 s total is still snappy enough to detect
// genuine link death well before user-visible impact.
static constexpr uint32_t ACK_TIMEOUT_MS = 1000;
static constexpr uint8_t  MAX_RETRIES    = 5;
// Payload bytes stored per slot for retransmit. Sized to HAP_MAX_PAYLOAD so
// that ACK-tracked responses like DEVICE_LIST / DEVICE_INFO aren't silently
// rejected at send time once networks grow large (Codex §3). Cost per side:
// WIN_SIZE * HAP_MAX_PAYLOAD = 16 * 4096 = 64 KB. We park `payload_copy`
// in PSRAM (P4 has 32 MB hex 200 MHz; S3 has its own SPIRAM partition)
// so the retransmit ring doesn't squat on internal SRAM. The buffer is
// touched only on `hap_session_send` (single memcpy from caller's
// payload, typically <256 B) and on retransmit (which is the slow
// path); PSRAM bandwidth is more than enough.
static constexpr size_t   SLOT_BUF_SIZE  = HAP_MAX_PAYLOAD;

// Defect 5 (FINDINGS §1.5): `hap_session_next_seq()` returns this when called
// before init. A frame carrying it must NEVER be sent — the send path rejects
// it with a hard error so a pre-init roundtrip fails fast instead of going on
// the wire with seq=0 (treated as "no correlation") and silently timing out.
// 0 is the natural choice: it is already the skip-0 / no-correlation value, so
// no legitimate outbound frame ever carries it.
static constexpr uint16_t SEQ_SENTINEL_UNINIT = 0;

struct WinSlot {
    bool     active;
    uint16_t seq;
    HapFrame frame;
    uint8_t* payload_copy;   // allocated from PSRAM in hap_session_init
    uint8_t  retries;
    // Defect 4 (FINDINGS §1.4): int64 monotonic µs→ms since boot. The old
    // uint32 ms (xTaskGetTickCount*portTICK_PERIOD_MS) wrapped at ~49.7 days
    // and could collide its sent_ms==0 sentinel with a real post-wrap tick of
    // 0 → one spurious retransmit per slot per wrap. int64 esp_timer never
    // wraps in any realistic uptime and is read ONLY for `active` slots
    // (the tick loop short-circuits inactive slots first), so no magic
    // sentinel is needed — `active` is the armed flag.
    int64_t  sent_ms;   // ms since boot when frame was last sent
};

static WinSlot            s_win[WIN_SIZE];
static uint16_t           s_next_seq = 1;
static HapSessionCfg      s_cfg;
static SemaphoreHandle_t  s_mutex;

// Defect 3 (FINDINGS §1.3): dedup against re-dispatch of NEEDS_ACK frames on
// peer retransmit. See README §"Receive-side dedup & sequence space" for the
// authoritative description. Two complementary mechanisms:
//
//  (a) Exact-match ring of recent (seq,type) pairs — catches the common case
//      (a frame we just handled is re-sent because our ACK was lost). The ring
//      was 16 entries; a burst of >16 distinct NEEDS_ACK frames between the
//      original dispatch and the retransmit evicted the entry, so the stale
//      frame slipped through and the handler ran twice (double DEVICE_DELETE /
//      double rule-create). Enlarged 16→64 (entries are 4 B each ⇒ 256 B) so
//      the window comfortably exceeds the peer's WIN_SIZE retransmit window.
//
//  (b) Monotonic high-water fast-path — tracks the highest seq seen per peer
//      and drops any frame that is more than WIN_SIZE behind it as a definite
//      stale duplicate, regardless of ring eviction. This is wrap-aware: seqs
//      are uint16 and roll 0xFFFF→0x0001, so we compare via the signed
//      difference trick (see seq_diff) rather than a raw `<`. The peer never
//      has more than WIN_SIZE (=peer-side) frames outstanding, so a seq that
//      far behind the high-water mark cannot be a fresh in-window frame.
struct SeenEntry { uint16_t seq; uint8_t type; };
static constexpr size_t SEEN_RING_SIZE = 64;
static SeenEntry s_seen_ring[SEEN_RING_SIZE] = {};
static uint8_t   s_seen_head = 0;

// Per-peer monotonic high-water of received NEEDS_ACK seqs. `valid` guards the
// cold-start case (no frame seen yet) so seq 0-vs-uninitialised is unambiguous.
static uint16_t s_rx_high_water = 0;
static bool     s_rx_high_water_valid = false;
// A frame this far (or more) behind the high-water mark is a definite stale
// dup. The peer's outstanding window is WIN_SIZE; allow a generous margin so a
// merely-reordered in-window frame is never mistaken for stale.
static constexpr int16_t STALE_BEHIND_THRESHOLD = WIN_SIZE;

// uint16 wrap-aware ordering. Returns a>b as a small signed delta: positive ⇒
// a is "ahead" of b, negative ⇒ "behind". Correct across the 0xFFFF→0x0001
// boundary because the subtraction wraps mod 2^16 and the int16_t cast
// re-centres it into [-32768, 32767]. e.g. seq_diff(0x0001, 0xFFFF) = +2,
// seq_diff(0xFFFF, 0x0001) = -2.
static inline int16_t seq_diff(uint16_t a, uint16_t b) {
    return static_cast<int16_t>(static_cast<uint16_t>(a - b));
}

static bool seen_recently(uint16_t seq, uint8_t type) {
    for (size_t i = 0; i < SEEN_RING_SIZE; i++) {
        if (s_seen_ring[i].seq == seq && s_seen_ring[i].type == type) return true;
    }
    return false;
}

// True ⇒ `seq` is far enough behind the monotonic high-water mark that it can
// only be a stale retransmit (burst-evicted from the ring). Wrap-safe.
static bool is_stale_behind_window(uint16_t seq) {
    if (!s_rx_high_water_valid) return false;
    return seq_diff(s_rx_high_water, seq) >= STALE_BEHIND_THRESHOLD;
}

static void mark_seen(uint16_t seq, uint8_t type) {
    s_seen_ring[s_seen_head] = {seq, type};
    s_seen_head = (s_seen_head + 1) % SEEN_RING_SIZE;
    // Advance the high-water mark only forward (wrap-aware): a freshly accepted
    // in-window frame may carry a seq slightly ahead of any seen so far.
    if (!s_rx_high_water_valid || seq_diff(seq, s_rx_high_water) > 0) {
        s_rx_high_water = seq;
        s_rx_high_water_valid = true;
    }
}

static int64_t now_ms() {
    return esp_timer_get_time() / 1000;
}

static int find_slot_by_seq(uint16_t seq) {
    for (int i = 0; i < WIN_SIZE; i++)
        if (s_win[i].active && s_win[i].seq == seq) return i;
    return -1;
}

static int find_free_slot() {
    for (int i = 0; i < WIN_SIZE; i++)
        if (!s_win[i].active) return i;
    return -1;
}

void hap_session_init(const HapSessionCfg& cfg) {
    // Q19 (QWEN_FINDINGS triage): guard against re-init. The original
    // unconditionally re-created s_mutex AND memset s_win (nulling the old
    // payload_copy pointers) AND re-allocated the 16 retransmit buffers — a
    // second call (e.g. a radio reset / self-heal path) leaked the mutex plus
    // 64 KB of PSRAM. Allocate the mutex + buffers once; on re-init just refresh
    // cfg and reset the per-slot bookkeeping, keeping the existing buffers.
    const bool first = (s_mutex == nullptr);
    if (first) {
        s_mutex = xSemaphoreCreateMutex();
        configASSERT(s_mutex);
    }
    s_cfg      = cfg;
    s_next_seq = 1;
    // Fresh session: reset the receive-side dedup high-water and ring so a
    // stale mark from a prior session (which reset s_next_seq back to 1) can't
    // wrongly classify the first post-init frames as "behind the window".
    // (hap_session_reset_link deliberately PRESERVES these — seq continuity is
    // kept there; only a full re-init starts the seq space over.)
    s_rx_high_water       = 0;
    s_rx_high_water_valid = false;
    s_seen_head           = 0;
    memset(s_seen_ring, 0, sizeof(s_seen_ring));
    for (int i = 0; i < WIN_SIZE; i++) {
        if (first) {
            // 16 × 4 KB parked in PSRAM (fallback to internal for non-PSRAM
            // targets / unit tests) so the retransmit ring is off internal SRAM.
            s_win[i].payload_copy = static_cast<uint8_t*>(
                heap_caps_malloc(SLOT_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (!s_win[i].payload_copy) {
                s_win[i].payload_copy = static_cast<uint8_t*>(
                    heap_caps_malloc(SLOT_BUF_SIZE, MALLOC_CAP_8BIT));
            }
            configASSERT(s_win[i].payload_copy);
        }
        s_win[i].active  = false;
        s_win[i].seq     = 0;
        s_win[i].retries = 0;
        s_win[i].sent_ms = 0;
    }
    ESP_LOGI(TAG, "win slots: %u × %zu B in PSRAM (%u KB)",
             WIN_SIZE, SLOT_BUF_SIZE,
             (unsigned)(WIN_SIZE * SLOT_BUF_SIZE / 1024));
}

void hap_session_reset_link() {
    if (!s_mutex) return;  // never inited — nothing in flight
    // Free every in-flight slot. The single frame that hit MAX_RETRIES is
    // already freed by hap_session_tick before on_link_dead fires, but
    // siblings enqueued just before the outage stay `active` and keep
    // retransmitting stale pre-outage seqs (the peer stopped tracking them),
    // which can wedge the window full after recovery. Mirror the init reset
    // of the per-slot bookkeeping; keep cfg, mutex, retransmit buffers and the
    // SEEN_RING (dedup is harmless across a resync) and DO NOT touch s_next_seq
    // — seq continuity is intentional, the peer does not expect a reset.
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int freed = 0;
    for (int i = 0; i < WIN_SIZE; i++) {
        if (s_win[i].active) freed++;
        s_win[i].active  = false;
        s_win[i].seq     = 0;
        s_win[i].retries = 0;
        s_win[i].sent_ms = 0;
    }
    xSemaphoreGive(s_mutex);
    if (freed) ESP_LOGW(TAG, "reset_link — abandoned %d in-flight frame(s)", freed);
}

uint16_t hap_session_next_seq() {
    if (!s_mutex) {
        // Defect 5 (FINDINGS §1.5): called before hap_session_init() — a
        // caller (e.g. a Lua script) that beat the init order. Previously we
        // returned 0 and let the frame go out; with seq=0 the peer treats it
        // as "no correlation", so a roundtrip waiting on that seq silently
        // timed out (and ate a full timeout before surfacing). Return the
        // explicit uninit sentinel instead — hap_session_send() rejects it
        // with ESP_ERR-class failure so the bug fails fast at the call site.
        ESP_LOGE(TAG, "next_seq before init — returning uninit sentinel");
        return SEQ_SENTINEL_UNINIT;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint16_t s = s_next_seq++;
    if (s_next_seq == 0) s_next_seq = 1;  // skip 0
    xSemaphoreGive(s_mutex);
    return s;
}

bool hap_session_send(const HapFrame& frame) {
    if (!s_cfg.send) {
        ESP_LOGE(TAG, "send() before hap_session_init — drop");
        return false;
    }
    // Defect 5 (FINDINGS §1.5): a seq of SEQ_SENTINEL_UNINIT can only come from
    // hap_session_next_seq() being called before init. Refuse to put it on the
    // wire — fail fast here (the caller's send returns false / its roundtrip
    // returns an immediate error) rather than letting a seq=0 "no correlation"
    // frame go out and silently time out. This is the shared send path used by
    // BOTH peers (P4 hap_slave_send + S3 hap_master_send transports), so the
    // guard covers every producer on either chip — including the NO_ACK data
    // requests S3's hap_roundtrip_v2 correlates on.
    if (frame.seq == SEQ_SENTINEL_UNINIT) {
        ESP_LOGE(TAG, "send() with uninit seq=0 — rejecting (next_seq before init?) type=0x%02x",
                 static_cast<uint8_t>(frame.type));
        return false;
    }
    // SYNC and NO_ACK frames bypass the window — no locking needed for send callback
    if (frame.type == HapMsgType::SYNC || (frame.flags & HAP_FLAG_NO_ACK)) {
        s_cfg.send(frame);
        return true;
    }

    if (!(frame.flags & HAP_FLAG_NEEDS_ACK)) {
        s_cfg.send(frame);
        return true;
    }

    if (!s_mutex) {
        ESP_LOGE(TAG, "send() before mutex init — drop seq=%u", frame.seq);
        return false;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int slot = find_free_slot();
    if (slot < 0) {
        xSemaphoreGive(s_mutex);
        _METRIC_COUNTER_INC(METRIC_HAP_WINDOW_FULL, 1);
        ESP_LOGE(TAG, "window full — drop seq=%u type=0x%02x (all %u slots in flight)",
                 frame.seq, static_cast<uint8_t>(frame.type), WIN_SIZE);
        return false;
    }

    WinSlot& ws = s_win[slot];
    ws.active  = true;
    ws.seq     = frame.seq;
    ws.retries = 0;
    ws.sent_ms = now_ms();
    ws.frame   = frame;

    // Copy payload for retransmit
    if (frame.payload_len > 0 && frame.payload) {
        if (frame.payload_len > SLOT_BUF_SIZE) {
            ESP_LOGE(TAG, "payload too large for retransmit slot: %u > %zu",
                     frame.payload_len, SLOT_BUF_SIZE);
            ws.active = false;
            xSemaphoreGive(s_mutex);
            return false;
        }
        memcpy(ws.payload_copy, frame.payload, frame.payload_len);
        ws.frame.payload     = ws.payload_copy;
        ws.frame.payload_len = frame.payload_len;
    } else {
        ws.frame.payload     = nullptr;
        ws.frame.payload_len = 0;
    }

    HapFrame to_send = ws.frame;
    xSemaphoreGive(s_mutex);

    s_cfg.send(to_send);
    return true;
}

void hap_session_on_receive(const HapFrame& frame) {
    if (!s_mutex) {
        ESP_LOGE(TAG, "on_receive before init — drop seq=%u", frame.seq);
        return;
    }
    if (frame.type == HapMsgType::ACK) {
        // The slot is keyed on the ORIGINAL outbound frame's seq, which
        // the ACK echoes back in `ack_seq` (set by hap_make_reply). The
        // ACK's own `seq` is its own fresh sender-side identifier and
        // never matches a slot.
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        int slot = find_slot_by_seq(frame.ack_seq);
        if (slot >= 0) {
            s_win[slot].active = false;
            ESP_LOGD(TAG, "ACK ack_seq=%u — slot %d freed", frame.ack_seq, slot);
        } else {
            // Common-case noise: peer ACKs each of our retransmits, but
            // the first ACK already cleared the slot. Subsequent ACKs
            // hit no-match. Not an error — log at debug level only.
            ESP_LOGD(TAG, "ACK ack_seq=%u — no matching slot (seq=%u)",
                     frame.ack_seq, frame.seq);
        }
        xSemaphoreGive(s_mutex);
        return;
    }

    if (frame.type == HapMsgType::SYNC) {
        if (s_cfg.on_sync) s_cfg.on_sync(frame);
        return;
    }

    // Data frame — send ACK if required.
    //
    // Defect 3 (FINDINGS §1.3): drop-without-dispatch a duplicate NEEDS_ACK
    // frame so the handler never runs twice on a peer retransmit. A frame is a
    // duplicate if EITHER the recent (seq,type) ring still holds it (exact
    // recent dup) OR it is far behind the monotonic high-water mark (a dup that
    // was burst-evicted from the ring). We still re-send the ACK — the peer is
    // retransmitting precisely because our prior ACK was lost — but we do NOT
    // re-dispatch to on_frame.
    if ((frame.flags & HAP_FLAG_NEEDS_ACK) &&
        (seen_recently(frame.seq, static_cast<uint8_t>(frame.type)) ||
         is_stale_behind_window(frame.seq))) {
        HapFrame ack = hap_make_reply(frame, HapMsgType::ACK, HAP_FLAG_NO_ACK);
        ack.seq = hap_session_next_seq();
        s_cfg.send(ack);
        _METRIC_COUNTER_INC(METRIC_HAP_DUP_SEQ, 1);
        return;
    }

    if (frame.flags & HAP_FLAG_NEEDS_ACK) {
        HapFrame ack = hap_make_reply(frame, HapMsgType::ACK, HAP_FLAG_NO_ACK);
        ack.seq = hap_session_next_seq();
        s_cfg.send(ack);
        mark_seen(frame.seq, static_cast<uint8_t>(frame.type));
    }

    if (s_cfg.on_frame) s_cfg.on_frame(frame);
}

// Concurrency note (responds to Qwen §1.1 "session send race"):
//
// `hap_session_send()` and `hap_session_tick()` both release s_mutex before
// calling s_cfg.send(). At first glance this looks racy — two concurrent
// SPI transactions? — but it is safe because:
//
//   1. The transport layer underneath (`hap_master_send` on S3,
//      `hap_slave_send` on P4) has its own mutex, so only one SPI
//      transaction is ever in flight on a chip at a time.
//
//   2. The sliding window is indexed by `seq`. A retransmit of frame X and
//      a fresh send of frame Y occupy different window slots; they do not
//      collide. The session mutex guards only slot bookkeeping, not the
//      actual wire transit — that's the transport's job.
//
// Retransmit latency note (Qwen §5):
//
// Retransmits are collected under the mutex and sent afterwards. Worst case
// under a degraded link: WIN_SIZE * frame_transit ≈ 8 * 3 ms = 24 ms of
// synchronous work in the caller's task. task_hap tolerates this because
// it's not a hot path (retransmits fire only when the peer is late to ACK).
// If measurements ever show retransmit storms blocking REST, the fix is a
// dedicated TX worker task fed by a queue; we haven't needed it.
void hap_session_tick() {
    if (!s_mutex) return;  // tick scheduled before init — no-op until ready
    // int64 to match WinSlot.sent_ms (Defect 4): the timeout delta `ms -
    // ws.sent_ms` stays a small positive int64 (both are monotonic ms-since-
    // boot from esp_timer), so `< ACK_TIMEOUT_MS` (uint32, promoted to int64)
    // is exact — no truncation, no 49.7-day wrap.
    int64_t ms = now_ms();
    bool link_dead_fired = false;

    // Collect frames to retransmit outside the lock to avoid calling send() under mutex
    HapFrame retransmit_frames[WIN_SIZE];
    int retransmit_count = 0;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < WIN_SIZE; i++) {
        WinSlot& ws = s_win[i];
        if (!ws.active) continue;
        if (ms - ws.sent_ms < ACK_TIMEOUT_MS) continue;

        if (ws.retries >= MAX_RETRIES) {
            ESP_LOGE(TAG, "link dead — seq=%u type=0x%02x (%u retransmits failed)",
                     ws.seq, static_cast<uint8_t>(ws.frame.type),
                     (unsigned)MAX_RETRIES);
            ws.active = false;
            link_dead_fired = true;
            continue;
        }

        ws.retries++;
        ws.sent_ms = ms;
        ESP_LOGW(TAG, "retransmit seq=%u type=0x%02x try=%d", ws.seq,
                 static_cast<uint8_t>(ws.frame.type), ws.retries);
        retransmit_frames[retransmit_count++] = ws.frame;
    }
    xSemaphoreGive(s_mutex);

    for (int i = 0; i < retransmit_count; i++) {
        s_cfg.send(retransmit_frames[i]);
    }

    // Call on_link_dead after the loop to avoid re-entrancy
    if (link_dead_fired && s_cfg.on_link_dead) {
        s_cfg.on_link_dead();
    }
}
