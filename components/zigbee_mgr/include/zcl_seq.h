// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Monotonic ZCL transaction-sequence counter shared by every outbound
// ZCL frame the firmware builds. Wraps 0x01–0xFF (zero is reserved as
// "not set" by a few helpers).
//
// Previously lived in zcl_converter; relocated when the auto-generated
// converter pipeline was dropped.
#pragma once

#include <cstdint>

uint8_t zcl_seq_next();
