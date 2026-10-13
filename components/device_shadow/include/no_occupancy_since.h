// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// "No motion since" for any motion sensor (z2m option `no_occupancy_since`,
// one interval): once a sensor reports occupancy = false, the hub publishes
// `no_occupancy_since = <interval>` when that many seconds pass without
// motion. Motion stops the count, and after a published interval it
// publishes 0 -- rules fire on a change, so the next quiet period must go
// 0 -> <interval> again. Unlike z2m, a repeated "no motion" report does not
// restart a running count (a sensor that repeats its state would otherwise
// never reach the interval).
//
// Pure state machine, no ESP-IDF: device_shadow owns the timer and the
// publishing and does what each Step says. All-zero is the off state, and the
// shadow memsets its entries, so the members have no initializers (they would
// make the entry non-trivial).
#pragma once

#include <cstdint>

struct NoOccupancySince {
    uint16_t interval_s;   // 0 = off
    bool     counting;     // the timer is armed
    bool     done;         // <interval> published since the last motion
    bool     nonzero;      // the last value published was not 0

    struct Step {
        bool     arm     = false;   // start the timer for interval_s
        bool     disarm  = false;   // stop the timer
        bool     publish = false;   // publish no_occupancy_since = value
        uint16_t value   = 0;
    };

    Step on_occupancy(bool occupied) {
        Step s;
        if (occupied) {
            if (counting) { s.disarm = true; counting = false; }
            done = false;
            if (nonzero) { s.publish = true; s.value = 0; nonzero = false; }
            return s;
        }
        if (interval_s == 0 || counting || done) return s;
        counting = true;
        s.arm = true;
        return s;
    }

    Step on_timer() {
        Step s;
        if (!counting || interval_s == 0) { counting = false; return s; }
        counting = false;
        done     = true;
        nonzero  = true;
        s.publish = true;
        s.value   = interval_s;
        return s;
    }

    // A new interval applies from the next "no motion" report.
    Step set_interval(uint16_t seconds) {
        Step s;
        if (counting) { s.disarm = true; counting = false; }
        interval_s = seconds;
        done       = false;
        return s;
    }
};
