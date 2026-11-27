// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstdint>
#include <cstddef>
uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t* buf, uint32_t len);
