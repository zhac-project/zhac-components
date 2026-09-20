// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: Apache-2.0
//
// zap_clock.h -- when the wall clock can be trusted, and who may set it.
//
// No ZHAC board has a battery-backed clock. After power-on the time counts from
// 1970 until SNTP, the S3's TIME_SYNC (on the P4) or the web UI's `time.set`
// supplies it, so anything before 2020 means "not set yet".
#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>

inline constexpr time_t kZapClockSetAfter  = 1577836800;   // 2020-01-01 UTC
inline constexpr time_t kZapClockSetBefore = 4102444800;   // 2100-01-01 UTC, sanity bound

inline bool zap_clock_is_set(time_t now) { return now >= kZapClockSetAfter; }

// A time a client may hand over with `time.set`. The handlers only apply it
// while zap_clock_is_set() is false: a browser's clock may fill an empty clock
// but never moves one that SNTP, TIME_SYNC or an earlier time.set has set.
inline bool zap_clock_epoch_ok(int64_t epoch) {
    return epoch >= kZapClockSetAfter && epoch < kZapClockSetBefore;
}

// ── Time server ─────────────────────────────────────────────────────────
// Where SNTP asks. A hub on a network without internet access can be pointed
// at a local server (a router, a NAS, a Pi) so its clock survives power cuts
// without anyone opening the web UI. Empty setting = the public default.
inline constexpr const char* kZapNtpDefault = "pool.ntp.org";
inline constexpr size_t      kZapNtpHostMax = 64;   // host or address, with NUL

// A host name or address the settings API accepts: printable, no spaces,
// quotes or backslashes (they would also break the JSON status), and short
// enough for the buffer that holds it.
inline bool zap_ntp_host_ok(const char* host) {
    if (!host || !*host) return false;
    size_t n = 0;
    for (const char* p = host; *p; ++p, ++n) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c <= 0x20 || c >= 0x7f || c == '"' || c == '\\') return false;
    }
    return n < kZapNtpHostMax;
}

