// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host shim for the centralized task stack-size table. device_shadow_init
// passes zhac::stack::kDeviceShadow to xTaskCreatePinnedToCore (a no-op on the
// host — the task never runs). Any value links; the firmware value is mirrored
// for fidelity. kZapFlush is retained from the zap_store harness shape.
#pragma once
#include <cstdint>
namespace zhac { namespace stack {
constexpr uint32_t kDeviceShadow = 4096;
constexpr uint32_t kZapFlush     = 4096;
} }
