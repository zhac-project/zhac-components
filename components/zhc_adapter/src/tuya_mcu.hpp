// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The hub's answers to a Tuya MCU's own requests on 0xEF00, as z2m's tuyaBase
// gives them (zigbee-herdsman-converters lib/tuya.ts):
//   0x24 mcuSyncTime                -> 08 00 + UTC + local seconds, big-endian,
//                                      since 1970 (time_start 1) or 2000 (2)
//   0x25 mcuGatewayConnectionStatus -> 01 00 01, "connected"
// A status query left unanswered is what an LCD sensor shows as "connection
// lost". Pure C++, no ESP-IDF, so it has a host test (test/host).
#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>

#include "zap_clock.h"

namespace tuya_mcu {

// The command id to answer the request frame `zcl` (ZCL header + payload, as
// received on 0xEF00) with, its payload in `out` / `out_len`; 0 = no answer.
// Time goes out only for a time_start (z2m's default is "off") and once our
// clock is set -- a 1970 answer would be worse than none. Local = UTC + the
// offset of the TZ the port set (none set: local == UTC).
inline std::uint8_t reply(const std::uint8_t* zcl, std::size_t len, std::uint8_t time_start,
                          std::time_t now, std::uint8_t (&out)[10], std::size_t& out_len) {
    out_len = 0;
    if (!zcl || len < 3 || (zcl[0] & 0x03) != 0x01) return 0;   // cluster-specific only
    const std::size_t at = (zcl[0] & 0x04) ? 4 : 2;              // a manufacturer code comes first
    if (len <= at) return 0;
    if (zcl[at] == 0x25) {
        out[0] = 0x01; out[1] = 0x00;   // payloadSize 1 (u16 LE)
        out[2] = 0x01;                  // connected
        out_len = 3;
        return 0x25;
    }
    if (zcl[at] != 0x24 || !time_start || !zap_clock_is_set(now)) return 0;
    struct tm g{};
    gmtime_r(&now, &g);
    g.tm_isdst = -1;
    const long gmtoff = static_cast<long>(now - mktime(&g));
    const std::uint32_t utc = static_cast<std::uint32_t>(now - (time_start == 2 ? 946684800 : 0));
    const std::uint32_t local = static_cast<std::uint32_t>(utc + gmtoff);
    out[0] = 0x08; out[1] = 0x00;       // payloadSize 8 (u16 LE)
    for (int i = 0; i < 4; ++i) {
        out[2 + i] = static_cast<std::uint8_t>(utc >> (24 - 8 * i));
        out[6 + i] = static_cast<std::uint8_t>(local >> (24 - 8 * i));
    }
    out_len = 10;
    return 0x24;
}

}  // namespace tuya_mcu
