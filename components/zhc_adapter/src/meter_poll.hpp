// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Periodic meter reads for plugs that never report their meter.
//
// A definition opts in with `PreparedDefinition::meter_poll` (z2m
// `tuya.modernExtend.electricityMeasurementPoll`). Every interval the hub
// reads haElectricalMeasurement [rmsVoltage, rmsCurrent, activePower] and/or
// seMetering [currentSummDelivered] on EP1; the Read Attributes Response
// comes back through the normal decode path like any report.
//
// One table, one tick: every known device has a slot (flags 0 = looked up,
// never polled, so its definition is not searched again). A new device's
// first read lands at a per-IEEE offset inside the interval so plugs added
// together (or all of them after a reboot) do not fire together, and a tick
// hands out at most kMaxPerTick devices. A device whose read could not be
// sent stays due and goes again next tick. Pure C++, no ESP-IDF, so the
// policy has a host test (test/host); the adapter adds the lock, the task,
// the device pool and the radio.
#pragma once

#include <cstddef>
#include <cstdint>

namespace zhac_meter {

// Same values as zhc::kMeterPollElectrical / kMeterPollMetering (the adapter
// static_asserts it; this header stays free of the library for the host test).
constexpr std::uint8_t kElectrical = 0x01;
constexpr std::uint8_t kMetering   = 0x02;

constexpr std::size_t   kSlots      = 200;          // == ZAP_MAX_DEVICES
constexpr std::uint32_t kTickMs     = 5u * 1000u;
// ponytail: fixed per-tick cap, no response tracking. 2 devices / 5 s bounds
// the radio load; with more than ~24 polled plugs the effective interval
// simply stretches past 60 s. Track outstanding TSNs if that ever matters.
constexpr std::size_t   kMaxPerTick = 2;

// ZCL Basic powerSource (bit 7 = backup battery): mains, 3-phase, DC,
// emergency mains. 0x03 battery and 0x00 unknown are not.
bool mains_powered(std::uint8_t power_source);

struct Read {
    std::uint16_t cluster;
    std::uint8_t  count;            // attribute ids in attrs_le
    std::uint8_t  attrs_le[6];      // 2 bytes each, little-endian
};

// The reads one poll issues for `flags`. Returns how many (0..2).
std::size_t reads_for(std::uint8_t flags, Read out[2]);

// Offset of a device's first poll inside the interval, derived from its IEEE.
std::uint32_t stagger(std::uint64_t ieee, std::uint32_t interval_ms);

struct Due {
    std::uint64_t ieee;
    std::uint16_t nwk;
    std::uint8_t  flags;
};

class Scheduler {
public:
    // Track a device. `flags` 0 = never poll it. A known IEEE only gets its
    // nwk and flags updated (its schedule stays). False if the table is full.
    bool add(std::uint64_t ieee, std::uint16_t nwk, std::uint8_t flags,
             std::uint32_t now_ms, std::uint32_t interval_ms);
    bool known(std::uint64_t ieee) const;
    void forget(std::uint64_t ieee);

    // Mark-and-sweep against the platform's device pool: begin_scan, then
    // refresh() every device still there (false = unknown, add it), then
    // end_scan drops the ones that were not.
    void begin_scan();
    bool refresh(std::uint64_t ieee, std::uint16_t nwk);
    void end_scan();

    // Up to `max` polled devices due at `now_ms`. They stay due until
    // polled() is called for them.
    std::size_t take_due(std::uint32_t now_ms, Due* out, std::size_t max);
    void polled(std::uint64_t ieee, std::uint32_t now_ms, std::uint32_t interval_ms);

private:
    struct Slot {
        std::uint64_t ieee;         // 0 = free
        std::uint32_t due_ms;
        std::uint16_t nwk;
        std::uint8_t  flags;
        bool          seen;
    };
    Slot* find(std::uint64_t ieee);
    Slot slots_[kSlots] = {};
};

}  // namespace zhac_meter
