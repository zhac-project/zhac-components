# hap_session — HAP Sliding-Window Reliability

ACK / retransmit layer that sits between `hap_protocol` (frame codec) and the
SPI transport (`hap_master` on S3, `hap_slave` on P4). Without `hap_session`,
a single dropped SPI byte loses an entire HAP command. With it, every
`HAP_FLAG_NEEDS_ACK` frame survives up to three retransmits before the link
is declared dead.

Despite the name, the session has no negotiation handshake — it is purely a
window-tracker. The `SYNC` frame (0x05) is processed but bypasses the window;
its job is informational ("I just rebooted, here's my fw rev").

## Where it sits

```
caller (hap_dispatch on P4, api_handlers on S3)
      │
      ▼
hap_session_send(frame)   ◀── this component
      │
      ▼
hap_master_send (S3)  /  hap_slave_send (P4)
      │
      ▼
SPI bus
      │
      ▼ (RX)
hap_session_on_receive(frame)   ── routes ACK/SYNC/data
      │
      └── on_frame / on_sync / on_link_dead callbacks
```

Both chips link `hap_session`. The S3 calls `hap_session_tick()` from
`task_hap` every ~10 ms; the P4's `task_hap` does the same.

## Dependencies (CMakeLists.txt)

`REQUIRES hap_protocol freertos` — needs the frame struct, mutex, and
`xTaskGetTickCount()` for ms timekeeping.

## Important constants (compile-time)

All four are `static constexpr` in `hap_session.cpp` — none are runtime
configurable on purpose; tuning them requires understanding the SPI link
timing budget.

| Constant         | Value             | Notes                                     |
|------------------|-------------------|-------------------------------------------|
| `WIN_SIZE`       | 16                | In-flight ACK-tracked frames per side.    |
| `ACK_TIMEOUT_MS` | 100               | Wait before first retransmit.             |
| `MAX_RETRIES`    | 3                 | After this, fires `on_link_dead`.         |
| `SLOT_BUF_SIZE`  | `HAP_MAX_PAYLOAD` | Per-slot retransmit buffer = 4096 B.      |

Memory cost per side: `WIN_SIZE * SLOT_BUF_SIZE = 16 * 4096 = 64 KB`. The
slots are parked in PSRAM (`MALLOC_CAP_SPIRAM`, internal fallback for
non-PSRAM builds), not BSS. This is sized for the worst case (`DEVICE_LIST`
on a 100-device network); the trade-off is documented in
`hap_session.cpp:29-37`. Acceptable on S3 (8 MB PSRAM) and P4.

## Receive-side dedup & sequence space

This is the authoritative description of how the RX path keeps a peer's
retransmits from being dispatched twice. The inline comment block in
`hap_session.cpp` (around the `seq_diff` / `SEEN_RING` region) points here;
keep this section in sync if the mechanics change.

### Sequence space

- Seqs are **`uint16_t`** and roll `0xFFFF → 0x0001`. `hap_session_next_seq()`
  **skips 0** — it never hands out 0 as a live seq.
- **0 doubles as `SEQ_SENTINEL_UNINIT`** ("uncorrelated / pre-init"). It is the
  value returned if `hap_session_next_seq()` is somehow called before
  `hap_session_init()`, and a frame carrying seq 0 is **rejected by
  `hap_session_send()`** rather than placed on the wire. Because the live space
  is `1..0xFFFF`, 0 is unambiguously "no real seq".

### Two-stage duplicate filter

A received `NEEDS_ACK` frame is checked against two complementary mechanisms
before it reaches `cfg.on_frame`:

1. **`SEEN_RING` — 64-entry exact-match ring keyed `(seq, type)`.** Catches the
   common case: a frame we just handled is re-sent because our ACK was lost.
   The ring covers the recent past; a burst of >64 distinct `NEEDS_ACK` frames
   between the original dispatch and the retransmit can evict an entry, which is
   why mechanism (2) exists as a backstop.
2. **High-water monotonic-seq fast-path.** The session tracks the highest seq
   received per peer (`s_rx_high_water`, gated by a `valid` flag for cold
   start). Any frame **`STALE_BEHIND_THRESHOLD (= WIN_SIZE)` or more behind** the
   high-water mark is dropped as a definite stale dup, regardless of ring
   eviction — the peer never has more than `WIN_SIZE` frames outstanding, so a
   seq that far behind cannot be a fresh in-window frame.

### Wrap-safe comparison (`seq_diff`)

Both the high-water advance and the stale test compare seqs through
**`seq_diff(a, b)` — an `int16_t` recentering** of `(a - b) mod 2^16` into
`[-32768, 32767]`. Positive ⇒ `a` is ahead of `b`, negative ⇒ behind. This is
correct across the `0xFFFF → 0x0001` boundary where a raw `<` would not be
(e.g. `seq_diff(0x0001, 0xFFFF) = +2`).

### Dropped dups still re-ACK

A frame dropped by either filter is **not** re-dispatched to `cfg.on_frame`,
but the session **still re-sends the ACK**. The peer is retransmitting
precisely because its previous ACK was lost; re-ACKing makes it stop, while the
handler runs exactly once.

### Lifecycle vs `reset_link`

- `hap_session_reset_link()` (re-SYNC) **preserves** the high-water mark and
  `SEEN_RING`, and does **not** touch `s_next_seq` — so seq continuity and dedup
  carry across a link resync.
- Only the full `hap_session_init()` resets the dedup state: `s_next_seq → 1`,
  `s_rx_high_water → 0`, `s_rx_high_water_valid → false`. This avoids a stale
  high-water from a prior session (whose `s_next_seq` also reset to 1) rejecting
  fresh low-numbered frames.

## Public API (`include/hap_session.h`)

```cpp
inline constexpr uint8_t HAP_FLAG_NEEDS_ACK = 0x01;
inline constexpr uint8_t HAP_FLAG_NO_ACK    = 0x02;

using HapSendFn       = std::function<void(const HapFrame&)>;
using HapFrameHandler = std::function<void(const HapFrame&)>;

struct HapSessionCfg {
    HapSendFn             send;          // hap_master_send or hap_slave_send
    HapFrameHandler       on_frame;      // validated app frame
    HapFrameHandler       on_sync;       // SYNC (peer rebooted)
    std::function<void()> on_link_dead;  // fired after MAX_RETRIES failures
};

// Reset all 16 window slots, install callbacks. Call once at boot, after
// hap_master_init / hap_slave_init.
void hap_session_init(const HapSessionCfg& cfg);

// Queue frame for send. NEEDS_ACK frames take a window slot; NO_ACK / SYNC
// / ACK frames bypass the window. Returns false ONLY if the window is full
// (16 NEEDS_ACK frames in flight). Caller's payload is copied into the slot.
bool hap_session_send(const HapFrame& frame);

// Feed a decoded frame from the transport's RX path. Routes:
//   ACK            → free the matching window slot (by SEQ).
//   SYNC           → invoke cfg.on_sync, no ACK sent.
//   anything else  → if NEEDS_ACK, emit ACK; then invoke cfg.on_frame.
void hap_session_on_receive(const HapFrame& frame);

// Periodic tick. Walks all 16 slots; for each that has been waiting longer
// than ACK_TIMEOUT_MS, retransmit (++retries, reset sent_ms). After
// MAX_RETRIES failures, mark the slot inactive and fire on_link_dead.
// Call every ~10 ms from a HAP-dedicated task; safe to call more often.
void hap_session_tick();

// Returns the next outgoing seq and increments the internal counter.
// Wraps from 0xFFFF → 1 (skips 0; 0 is reserved for SYNC and "no ack_seq").
uint16_t hap_session_next_seq();
```

## Wire interaction

```
Sender (S3)                              Receiver (P4)
   │                                          │
   │── seq=1, flags=NEEDS_ACK, GET_DEVICES ──▶│
   │   slot 0: active=1, retries=0           │
   │   sent_ms = now                          │
   │                                          │── on_receive: seq=1, NEEDS_ACK
   │                                          │  → emit ACK(seq=1), then
   │                                          │     cfg.on_frame(frame)
   │◀── seq=1, type=ACK, flags=0 ────────────│
   │   on_receive: type=ACK, seq=1            │
   │   → free slot 0                          │
   │                                          │
   │   ... 100 ms passes, no ACK arrived ...  │
   │── tick: slot 0 sent_ms - now > 100 ──────│ retransmit, retries=1
   │── (still no ACK) ── tick ─── retries=2 ─│
   │── (still no ACK) ── tick ─── retries=3 ─│
   │── (still no ACK) ── tick ──────────────▶│
   │       link dead — fires on_link_dead    │
```

## Threading and concurrency

Single internal mutex (`s_mutex`) guards the 16-slot window array. **Both
`hap_session_send()` and `hap_session_tick()` release `s_mutex` before
calling the user's `send` callback** to avoid holding a lock across the
SPI transit. This is safe because:

1. The transport (`hap_master_send`, `hap_slave_send`) has its own
   internal serialization (mutex on S3; single dedicated task on P4) — only
   one SPI transaction is ever in flight per chip.
2. The window is keyed by `seq`; a retransmit of seq X and a fresh send
   of seq Y occupy different slots and never collide.

The session mutex protects only slot bookkeeping, not the wire transit
itself. See the inline `hap_session.cpp:144-167` comment block for the full
rationale (and the upper bound on retransmit-storm latency: `WIN_SIZE *
frame_transit ≈ 48 ms` of synchronous work in the caller's task — non-issue
on `task_hap`).

`on_link_dead` is fired **outside** the mutex and after the retransmit loop
to avoid re-entrancy if the user's handler decides to `hap_session_send()`
again.

## Error and failure modes

| Log line                                          | Meaning                                              |
|---------------------------------------------------|------------------------------------------------------|
| `W window full — drop seq=N type=0xXX`            | 16 NEEDS_ACK frames in flight; new send returns false. Caller usually drops or retries from app layer. |
| `E payload too large for retransmit slot: N > M`  | Encoded frame > `SLOT_BUF_SIZE`. Should be impossible after the v2 widening; would indicate a `HAP_MAX_PAYLOAD` bump that didn't propagate. |
| `W retransmit seq=N type=0xXX try=k`              | Per-attempt warning. Three of these in a row precede a link-dead. |
| `E link dead — seq=N type=0xXX (3 retransmits failed)` | Final retransmit timed out; `on_link_dead` callback fires. Most callers reboot the link. |
| `D ACK seq=N — slot k freed`                      | Debug — successful ACK round-trip.                  |

## Integration example

Minimal S3-side init (the P4 side is symmetric — swap `hap_master_*` for
`hap_slave_*`):

```cpp
#include "hap_session.h"
#include "hap_master.h"

static void on_frame(const HapFrame& f) {
    // Dispatch by f.type — see api_handlers / hap_dispatch.
}

static void on_sync(const HapFrame&)  { ESP_LOGI("hap", "P4 rebooted"); }
static void on_link_dead()            { esp_restart(); /* or reset SPI */ }

void hap_init() {
    hap_master_init();
    hap_master_set_callback([](const HapFrame& f) { hap_session_on_receive(f); });

    HapSessionCfg cfg{
        .send         = [](const HapFrame& f) { hap_master_send(f); },
        .on_frame     = on_frame,
        .on_sync      = on_sync,
        .on_link_dead = on_link_dead,
    };
    hap_session_init(cfg);
}

// In task_hap (10 ms cadence)
for (;;) {
    hap_session_tick();
    vTaskDelay(pdMS_TO_TICKS(10));
}
```

## Cross-references

- `docs/FINDINGS.md` — link-dead handling rationale
- `components/hap_protocol/README.md` — frame layout, ACK_SEQ semantics
- `components/hap_master/README.md`, `components/hap_slave/README.md` — transport
- `zhac-main-core/main/hap_dispatch.cpp` — example consumer (P4)

## Recent changes

- `SLOT_BUF_SIZE` was widened to `HAP_MAX_PAYLOAD` (4 KB) so large ACK-tracked
  responses (`DEVICE_LIST` / `DEVICE_INFO` on 100-device networks) no longer
  hit the "payload too large for retransmit slot" guard.
- `WIN_SIZE` raised to 16 — earlier caps (4, then 8) throttled S3 batched
  reads under load.
- Sender now releases the session mutex before calling the transport, fixing
  the long-tail retransmit latency observed when SPI was slow to drain. See
  `hap_session.cpp:144-167`.
