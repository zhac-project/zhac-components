// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host tests for zap_store P2 (FINDINGS §8) capacity + CRC logic.
//
// zap_store.cpp itself pulls in NVS + FreeRTOS, so it cannot link on the
// host. These tests exercise the two pieces of logic that are PURE — the
// capacity-reject decision and the CRC-in-place equivalence — by mirroring
// the exact predicates from the patched source against the REAL ZapDevice
// layout (from zap_common.h) and a real CRC32 (from the stub). If the
// source predicate drifts from what is asserted here, this test is the
// canary; keep them in lockstep.
//
//   §8.1 capacity reject: a NEW device at a full store is rejected BEFORE
//                         any write/count-bump; an EXISTING device is an
//                         in-place rewrite and never rejected.
//   §8.4 CRC in-place:    crc over a pre-zeroed copy (save path) ==
//                         crc over an arbitrary struct with the field
//                         zeroed in a defensive copy (load path).
#include "zap_common.h"
#include "esp_rom_crc.h"

#include <cstdio>
#include <cstring>
#include <cstdint>

static int s_failures = 0;
#define CHECK(cond, msg) do {                                              \
    if (!(cond)) { s_failures++; printf("  FAIL: %s\n", msg); }            \
    else         { printf("  ok:   %s\n", msg); }                          \
} while (0)

// ── Mirror of the patched zap_store.cpp CRC helpers (FINDINGS §8.4) ───────
// zap_device_crc_zeroed: NO defensive copy — caller already zeroed crc32.
static inline uint32_t zap_device_crc_zeroed(const ZapDevice* d) {
    return esp_rom_crc32_le(0, reinterpret_cast<const uint8_t*>(d), sizeof(*d));
}
// zap_device_crc: load path — zeroes a copy so the stored crc32 is excluded.
static uint32_t zap_device_crc(const ZapDevice* d) {
    ZapDevice tmp;
    memcpy(&tmp, d, sizeof(tmp));
    tmp.crc32 = 0;
    return zap_device_crc_zeroed(&tmp);
}

// ── Mirror of the patched save-path capacity decision (FINDINGS §8.1) ─────
// Returns true if the save would be REJECTED (store full + new device).
static bool save_would_reject(int16_t found_idx, uint16_t count) {
    return found_idx < 0 && count >= ZAP_MAX_DEVICES;
}
// The index-sync guard that prevents an out-of-bounds write to s_idx_ieee[].
static bool idx_in_index_bounds(uint16_t idx) {
    return idx < ZAP_MAX_DEVICES;
}

int main() {
    printf("test_zap_store_logic (FINDINGS §8 capacity + CRC)\n");

    // ── §8.1: capacity-reject decision table ──────────────────────────────
    {
        // New device, store has room → accept.
        CHECK(!save_would_reject(-1, 0),            "empty store accepts new device");
        CHECK(!save_would_reject(-1, 199),          "store at 199/200 accepts new device");

        // New device, store FULL → reject (this is the leak the task fixes).
        CHECK(save_would_reject(-1, ZAP_MAX_DEVICES),       "full store REJECTS new device");
        CHECK(save_would_reject(-1, ZAP_MAX_DEVICES + 1),   "over-full store REJECTS new device");

        // Existing device → in-place rewrite, NEVER rejected, even at cap.
        CHECK(!save_would_reject(0,   ZAP_MAX_DEVICES),     "existing device rewrites at capacity (idx 0)");
        CHECK(!save_would_reject(199, ZAP_MAX_DEVICES),     "existing device rewrites at capacity (idx 199)");
    }

    // ── §8.1: the idx chosen for a NEW device that PASSES the guard is
    //          always within the index array bounds. The reject above is the
    //          ONLY thing that keeps idx == count < ZAP_MAX_DEVICES here, so
    //          the index-sync write s_idx_ieee[idx] cannot overflow. ────────
    {
        for (uint16_t count = 0; count < ZAP_MAX_DEVICES; count++) {
            // new device passes the reject → idx = count
            if (save_would_reject(-1, count)) { s_failures++; printf("  FAIL: count<MAX wrongly rejected\n"); break; }
            uint16_t idx = count;  // found_idx < 0 path
            if (!idx_in_index_bounds(idx)) { s_failures++; printf("  FAIL: idx %u OOB\n", idx); break; }
        }
        CHECK(true, "every accepted new-device idx (0..199) stays in index bounds");
        // At capacity the would-be idx == 200 is OOB — and is exactly what
        // the reject prevents from ever reaching the index write.
        CHECK(!idx_in_index_bounds(ZAP_MAX_DEVICES),
              "idx == ZAP_MAX_DEVICES is OOB (must be unreachable post-reject)");
    }

    // ── §8.4: CRC in-place equals CRC-with-copy for the same logical bytes ─
    {
        ZapDevice d{};
        d.ieee_addr   = 0xAABBCCDDEEFF0011ULL;
        d.nwk_addr    = 0x1234;
        d.device_type = 7;
        strcpy(d.friendly_name, "TestBulb");
        d.endpoints[0] = 1;
        d.crc32        = 0xDEADBEEF;  // stale/garbage — must be excluded

        // Save path: hold a copy, zero crc32, CRC in place (no 2nd copy).
        ZapDevice saved;
        memcpy(&saved, &d, sizeof(saved));
        saved.crc32 = 0;
        uint32_t crc_inplace = zap_device_crc_zeroed(&saved);

        // Load path: the in-RAM record carries the stored crc; zap_device_crc
        // zeroes a copy and CRCs that. Feeding it the original (crc field set)
        // must yield the SAME value — the field is excluded either way.
        uint32_t crc_copy = zap_device_crc(&d);

        CHECK(crc_inplace == crc_copy,
              "CRC-in-place == CRC-with-copy (crc32 field excluded both ways)");

        // The CRC must not depend on the prior crc32 contents.
        ZapDevice d2 = d; d2.crc32 = 0x00000000;
        CHECK(zap_device_crc(&d) == zap_device_crc(&d2),
              "CRC independent of stored crc32 field value");

        // Sanity: a payload change DOES change the CRC.
        ZapDevice d3 = d; d3.nwk_addr = 0x9999;
        CHECK(zap_device_crc(&d) != zap_device_crc(&d3),
              "CRC changes when payload changes");

        // Round-trip: stamp the computed CRC, then a verify must match.
        saved.crc32 = crc_inplace;
        CHECK(saved.crc32 == zap_device_crc(&saved),
              "stamped record verifies (stored crc == recomputed)");
    }

    printf("\n%s — %d failure(s)\n", s_failures ? "FAILED" : "ALL PASS", s_failures);
    return s_failures ? 1 : 0;
}
