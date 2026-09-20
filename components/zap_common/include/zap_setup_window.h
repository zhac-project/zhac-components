// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: Apache-2.0
//
// zap_setup_window.h -- how long after power-on a hub with no admin password
// lets the first visitor set one.
//
// "First visitor claims the hub" is the usual onboarding pattern, but without
// a bound anyone who finds a hub that was never set up, or was reset weeks
// ago, owns it. No ZHAC board has a button the firmware can rely on, so
// physical presence is proven the way every board allows: by power-cycling
// it. The setup endpoint answers only in the first kZapSetupWindowS after
// boot; after that the web UI says "unplug the hub, plug it back in, then set
// the password within 10 minutes". A password, once set, closes it for good.
#pragma once

#include <cstdint>

#include "esp_timer.h"

inline constexpr uint32_t kZapSetupWindowS = 10 * 60;

// Seconds the setup window has left, 0 once it has closed. Callers gate on
// "no password yet" themselves.
inline uint32_t zap_setup_secs_left() {
    const int64_t up_s = esp_timer_get_time() / 1000000;
    return up_s >= static_cast<int64_t>(kZapSetupWindowS) ? 0u
                                                          : static_cast<uint32_t>(kZapSetupWindowS - up_s);
}
