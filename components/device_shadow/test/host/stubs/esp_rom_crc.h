// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: Apache-2.0
// Host-test esp_rom_crc shim — a real (reflected) CRC32 so the in-place vs
// copy CRC equivalence test (FINDINGS §8.4) is meaningful, not a tautology.
// The exact polynomial does not matter for the equivalence property; what
// matters is that the same bytes produce the same value. ESP-IDF's
// esp_rom_crc32_le is the reflected IEEE 802.3 CRC32.
#pragma once
#include <cstddef>
#include <cstdint>

static inline uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t* buf, uint32_t len) {
    crc = ~crc;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (-(int32_t)(crc & 1)));
    }
    return ~crc;
}
