// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// HOTFIX host tests — DEVICE_LIST paging (device.list timeout >~15 devices).
//
// STEP 1 (root-cause gate): build N synthetic ZapDevices with realistic
// field lengths and feed them to the CURRENT single-frame encoder into a
// HAP_MAX_PAYLOAD(4096) buffer. The bug claim is that the JSON exceeds 4096
// somewhere in 13..18 devices, so the encoder returns false, P4 logs
// "GET_DEVICES encode failed", never sends, and S3's roundtrip times out.
// This test FINDS and REPORTS the exact overflow threshold. If it does NOT
// overflow at a realistic device count (<=25), the harness fails loudly so
// we do NOT build paging on a false premise.
//
// STEP 3 (paging proof): the same synthetic devices are paged through the
// new (start_index/next_index) overload; we prove every chunk fits in 4096,
// the cursor strictly advances (forward progress), reassembly reproduces the
// full devices array, and the single-chunk + empty-list edges hold.

#include "hap_json.h"
#include "zap_common.h"
#include "zcl_attribute.h"   // VAL_FLOAT / VAL_INT

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

int g_esp_loge_count = 0;   // bumped by the ESP_LOGE stub

static int s_failures = 0;
#define CHECK(cond, msg) do {                                         \
    if (cond) { printf("PASS: %s\n", msg); }                          \
    else      { printf("FAIL: %s\n", msg); s_failures++; }            \
} while (0)

// ── Synthetic device factory ──────────────────────────────────────────────
// Field lengths chosen to mirror the worst-case real fleet (Tuya/_TZ3000_*
// manufacturer strings, long friendly names, full model ids). These are the
// strings that actually blow the 4096 budget on hardware.
static void fill_dev(ZapDevice& d, uint16_t i) {
    std::memset(&d, 0, sizeof(d));
    d.ieee_addr        = 0x00124B0001020300ULL + i;
    d.nwk_addr         = static_cast<uint16_t>(0x1000 + i);
    d.endpoint_count   = 1;
    d.device_type      = 1;
    d.protocol         = PROTO_ZIGBEE;
    d.flags            = 0;                       // visible
    d.last_seen        = 1717000000u + i;
    d.manufacturer_code = 4417;
    d.link_quality     = 200;
    d.power_source     = 1;
    d.battery_pct      = 87;
    // friendly_name[30]: ~20 chars of realistic "Living room ..." naming.
    std::snprintf(d.friendly_name, sizeof(d.friendly_name), "Living Room Lamp %02u", i);
    // manufacturer_name[34]: a full Tuya manu id (~16 chars) + index spread.
    std::snprintf(d.manufacturer_name, sizeof(d.manufacturer_name), "_TZ3000_xwh1e22x%02u", i);
    // model_id[34]: full model identifier.
    std::snprintf(d.model_id, sizeof(d.model_id), "TS0601_dimmer_%02u", i);
}

// Stub label resolver: returns ~20-char vendor/model strings, like a matched
// ZHC def would. This is the realistic case (resolver present, names long).
static void stub_resolver(const ZapDevice* /*dev*/, char* vbuf, size_t vcap,
                                                     char* mbuf, size_t mcap) {
    std::snprintf(vbuf, vcap, "Tuya Smart Lighting");   // 19 chars
    std::snprintf(mbuf, mcap, "Smart Dimmer Module");   // 19 chars
}

static std::vector<ZapDevice> make_fleet(uint16_t n) {
    std::vector<ZapDevice> v(n);
    for (uint16_t i = 0; i < n; i++) fill_dev(v[i], i);
    return v;
}

// Count "ieee" occurrences == number of device objects encoded in a chunk.
static int count_devices_in(const char* json, size_t len) {
    int c = 0;
    std::string s(json, len);
    size_t pos = 0;
    const std::string needle = "\"ieee\"";
    while ((pos = s.find(needle, pos)) != std::string::npos) { c++; pos += needle.size(); }
    return c;
}

int main() {
    constexpr size_t kCap = HAP_MAX_PAYLOAD;   // 4096 — one SPI frame
    uint8_t buf[kCap];

    printf("== STEP 1: confirm single-frame overflow threshold ==\n");
    // Sweep device counts; find the first N where the CURRENT single-frame
    // encoder (start_index path with start=0, all-or-nothing) returns false.
    int first_overflow = -1;
    size_t bytes_at_last_ok = 0;
    int last_ok_n = 0;
    for (uint16_t n = 1; n <= 40; n++) {
        auto fleet = make_fleet(n);
        uint16_t len = 0;
        g_esp_loge_count = 0;
        bool ok = hap_json_encode_device_list(buf, kCap, &len,
                                              fleet.data(), n, &stub_resolver);
        if (ok) {
            bytes_at_last_ok = len;
            last_ok_n = n;
        } else if (first_overflow < 0) {
            first_overflow = n;
            printf("  -> first overflow at N=%d devices "
                   "(last OK: N=%d at %zu bytes, ~%zu B/device)\n",
                   first_overflow, last_ok_n, bytes_at_last_ok,
                   last_ok_n ? bytes_at_last_ok / (size_t)last_ok_n : 0);
            CHECK(g_esp_loge_count >= 1,
                  "STEP1: overflow path logged ESP_LOGE (the silent-no-send bug)");
            break;
        }
    }

    CHECK(first_overflow > 0,
          "STEP1: single-frame encoder DOES overflow within 40 devices");
    // The bug report pins the failure at ~13..18 devices. Gate hard: if it
    // only overflows past 25, the premise is wrong — STOP, don't page.
    CHECK(first_overflow > 0 && first_overflow <= 25,
          "STEP1: overflow threshold is a realistic device count (<=25)");
    if (first_overflow >= 13 && first_overflow <= 18) {
        printf("  -> threshold %d is squarely in the reported 13..18 band\n",
               first_overflow);
    }

    printf("\n== STEP 3: paged encoder splits + reassembles ==\n");
    // Page a 30-device fleet (> threshold) through the new overload.
    auto big = make_fleet(30);
    {
        std::vector<ZapDevice> reassembled;
        std::vector<std::string> chunks;
        uint16_t start = 0;
        int guard = 0;
        bool forward_ok = true;
        bool fit_ok = true;
        while (start < 30 && guard++ < 100) {
            uint16_t len = 0;
            uint16_t next = start;   // sentinel: must change
            bool ok = hap_json_encode_device_list(buf, kCap, &len,
                                                  big.data(), 30,
                                                  &stub_resolver,
                                                  start, &next);
            if (!ok) { printf("  paged encode returned false at start=%u\n", start); fit_ok = false; break; }
            if (len > kCap) fit_ok = false;
            // forward progress: next strictly past start, never stuck.
            if (next <= start) { forward_ok = false; break; }
            int n_here = count_devices_in(reinterpret_cast<char*>(buf), len);
            (void)n_here;
            chunks.emplace_back(reinterpret_cast<char*>(buf), len);
            start = next;
        }
        CHECK(forward_ok, "STEP3: cursor strictly advances every chunk (forward progress)");
        CHECK(fit_ok,     "STEP3: every chunk <= HAP_MAX_PAYLOAD (4096)");
        CHECK(chunks.size() >= 2, "STEP3: 30 devices split into >=2 chunks");
        CHECK(start >= 30, "STEP3: paging terminates with cursor at count (done sentinel)");

        // Reassemble: total device count across chunks == 30, fields intact.
        int total = 0;
        bool saw_first = false, saw_last = false;
        for (auto& c : chunks) {
            total += count_devices_in(c.data(), c.size());
            if (c.find("Living Room Lamp 00") != std::string::npos) saw_first = true;
            if (c.find("Living Room Lamp 29") != std::string::npos) saw_last = true;
        }
        printf("  -> 30 devices => %zu chunks, %d device objects reassembled\n",
               chunks.size(), total);
        CHECK(total == 30, "STEP3: reassembled device count == 30 (no loss, no dupes)");
        CHECK(saw_first && saw_last,
              "STEP3: first + last device fields present across chunks (spot-check)");
    }

    printf("\n== STEP 3: edge cases ==\n");
    // Single chunk: small fleet that fits — one chunk, next == count.
    {
        auto few = make_fleet(3);
        uint16_t len = 0, next = 0;
        bool ok = hap_json_encode_device_list(buf, kCap, &len, few.data(), 3,
                                              &stub_resolver, 0, &next);
        CHECK(ok, "STEP3 edge: 3-device fleet encodes ok");
        CHECK(next == 3, "STEP3 edge: single chunk sets next == count (done)");
        CHECK(count_devices_in(reinterpret_cast<char*>(buf), len) == 3,
              "STEP3 edge: single chunk holds all 3");
    }
    // Empty list: count 0 -> valid {"devices":[]}, next == 0.
    {
        uint16_t len = 0, next = 5;
        bool ok = hap_json_encode_device_list(buf, kCap, &len, nullptr, 0,
                                              &stub_resolver, 0, &next);
        CHECK(ok, "STEP3 edge: empty fleet encodes ok");
        CHECK(next == 0, "STEP3 edge: empty fleet sets next == 0 (done)");
        std::string s(reinterpret_cast<char*>(buf), len);
        CHECK(s.find("\"devices\"") != std::string::npos &&
              s.find("\"ieee\"") == std::string::npos,
              "STEP3 edge: empty fleet emits {\"devices\":[]} shape");
    }
    // start_index past the end: nothing to encode, next pinned at count.
    {
        auto few = make_fleet(4);
        uint16_t len = 0, next = 0;
        bool ok = hap_json_encode_device_list(buf, kCap, &len, few.data(), 4,
                                              &stub_resolver, 4, &next);
        CHECK(ok, "STEP3 edge: start==count encodes ok (empty page)");
        CHECK(next == 4, "STEP3 edge: start==count keeps next==count (done)");
        CHECK(count_devices_in(reinterpret_cast<char*>(buf), len) == 0,
              "STEP3 edge: start==count emits zero devices");
    }

    // ── STEP4: type-driven value scaling in the live attr encoder. A
    // VAL_FLOAT attr unscales (/100); a genuine integer (VAL_INT) stays raw —
    // the type, not a hardcoded key list, decides. Guards the kFloatKeys removal
    // (which used to ÷100 an integer "humidity" → 0.49, and never unscaled the
    // snapshot path → temperature 2900).
    {
        uint8_t buf[512]; uint16_t len = 0;
        bool ok = hap_json_encode_device_attr_update(
            buf, sizeof(buf), &len, 0x1122334455667788ULL,
            "temperature", VAL_FLOAT, 2650, nullptr, 200, 0);
        std::string s(reinterpret_cast<char*>(buf), len);
        CHECK(ok, "STEP4: VAL_FLOAT attr encodes ok");
        CHECK(s.find("\"temperature\"") != std::string::npos &&
              s.find("26.5") != std::string::npos,
              "STEP4: VAL_FLOAT 2650 -> 26.5 (unscaled /100)");

        len = 0;
        ok = hap_json_encode_device_attr_update(
            buf, sizeof(buf), &len, 0x1122334455667788ULL,
            "humidity", VAL_INT, 42, nullptr, 200, 0);
        std::string s2(reinterpret_cast<char*>(buf), len);
        CHECK(ok, "STEP4: VAL_INT attr encodes ok");
        CHECK(s2.find("\"humidity\":42") != std::string::npos,
              "STEP4: integer humidity stays 42 (no key-list ÷100)");
        CHECK(s2.find("0.42") == std::string::npos,
              "STEP4: integer humidity is never ÷100'd");
    }

    printf("\n%s (%d failure%s)\n", s_failures ? "FAILED" : "ALL PASSED",
           s_failures, s_failures == 1 ? "" : "s");
    return s_failures;
}
