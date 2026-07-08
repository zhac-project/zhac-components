// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host shim for ESP-IDF section-placement attributes. device_shadow parks its
// large PSRAM scratch buffers behind EXT_RAM_BSS_ATTR; on the host these all
// collapse to ordinary BSS. Defined empty so the qualifiers vanish.
#pragma once
#define EXT_RAM_BSS_ATTR
#define IRAM_ATTR
#define DRAM_ATTR
#define RTC_DATA_ATTR
#define RTC_IRAM_ATTR
#define __NOINIT_ATTR
