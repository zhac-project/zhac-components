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
//
// The same table pings quiet devices, as zigbee2mqtt's availability does for
// "active" ones (receiver always on, see stays_awake): a device the hub has not
// heard from for the ping interval gets a genBasic zclVersion read, which is
// herdsman's Device.ping(). So a light that never reports, driven only by its
// own remote, still answers and keeps a fresh last_seen. An unanswered ping
// backs off as z2m's does: 1.5x the interval, then twice as long each time
// (10, 15, 30, 60, 120 min ... with the default 10 min), up to kPingMaxMs.
// Anything heard from the device resets it.
//
// And it says when a device is offline: a pinged one once it has sent nothing
// for kActiveOfflineMs (an hour) and left its last ping unanswered, one that is
// not pinged (battery, asleep between its own reports) once it has sent
// nothing for kPassiveOfflineMs (48 h) -- z2m's availability, with the hub
// owner's timeouts (2026-11-27). Its next frame brings it back.
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

constexpr std::uint32_t kPingJitterMs = 30u * 1000u;              // z2m max_jitter
constexpr std::uint32_t kPingMaxMs    = 24u * 60u * 60u * 1000u;  // longest backoff
constexpr std::uint32_t kOfflineMs    = 30u * 1000u;  // no frame this long after a ping: unanswered
                                                      // (herdsman's read timeout + its retry ~25 s)
constexpr std::uint32_t kActiveOfflineMs  = 60u * 60u * 1000u;        // pinged, silent this long: offline
constexpr std::uint32_t kPassiveOfflineMs = 48u * 60u * 60u * 1000u;  // not pinged, silent this long: offline

// Keeps its receiver on, so it is pinged: z2m's "active" devices, a router
// (node descriptor logical type 1) not on battery, or a device on single-phase
// mains (ZCL Basic powerSource 0x01; bit 7 = backup battery). The rest --
// battery, an end device on another or no known power source, an interview
// that learnt neither -- sleep between their own reports: never pinged,
// offline only after kPassiveOfflineMs.
bool stays_awake(std::uint8_t power_source, std::uint8_t logical_type);

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
    std::uint8_t  ep;               // ping endpoint (take_ping_due only)
};

class Scheduler {
public:
    // Track a device. `flags` 0 = never poll it. A non-zero `ping_interval_ms`
    // turns availability on (offline verdicts); with `awake` the device is also
    // pinged when quiet, on `ping_ep`. A known IEEE only gets its nwk, flags
    // and ping settings updated (its schedules and silence stay).
    // False if the table is full.
    bool add(std::uint64_t ieee, std::uint16_t nwk, std::uint8_t flags,
             std::uint32_t now_ms, std::uint32_t interval_ms,
             bool awake = false, std::uint8_t ping_ep = 1, std::uint32_t ping_interval_ms = 0);
    bool known(std::uint64_t ieee) const;
    void forget(std::uint64_t ieee);

    // Mark-and-sweep against the platform's device pool: begin_scan, then
    // refresh() every device still there (false = unknown, add it), then
    // end_scan drops the ones that were not. refresh() takes the device's
    // class again (`awake`), so a re-interview that learns it applies without
    // a restart. ponytail: its meter flags stay as first looked up (polled
    // plugs are mains routers from the start); look them up again on a class
    // change if that ever matters.
    void begin_scan();
    bool refresh(std::uint64_t ieee, std::uint16_t nwk, bool awake);
    void end_scan();

    // Up to `max` polled devices due at `now_ms`. They stay due until
    // polled() is called for them.
    std::size_t take_due(std::uint32_t now_ms, Due* out, std::size_t max);

    // Reschedules the device for `interval_ms` from now, regardless of `ok`
    // (I-2: a failed read backs off exactly like a successful one, instead
    // of retrying every kTickMs). Returns true the first time in a row that
    // `ok` is false -- the caller should log a warning once for the new
    // failure streak; a success (`ok` true, the default) always resets the
    // streak and returns false.
    bool polled(std::uint64_t ieee, std::uint32_t now_ms, std::uint32_t interval_ms,
                bool ok = true);

    // The device just sent something: its silence starts over, its next ping
    // is a full interval away and the backoff starts over. True when this ends
    // a silence of at least the interval (in practice: the answer to a ping),
    // once per silence, or brings an offline device back.
    bool heard(std::uint64_t ieee, std::uint32_t now_ms, std::uint32_t interval_ms);

    // Up to `max` devices to ping at `now_ms`. They stay due until pinged().
    std::size_t take_ping_due(std::uint32_t now_ms, Due* out, std::size_t max);

    // A ping went out (or could not): the next one waits the backed-off
    // interval unless heard() comes first. Returns true when the ping before
    // this one went unanswered, once per silence -- the caller logs it.
    bool pinged(std::uint64_t ieee, std::uint32_t now_ms, std::uint32_t interval_ms);

    // Devices offline now, each said once per silence; heard() brings one back.
    // A pinged device: silent for kActiveOfflineMs, its last ping unanswered
    // for kOfflineMs. One that is not pinged: silent for kPassiveOfflineMs.
    // Only while availability is on (add() with a ping interval).
    std::size_t take_offline(std::uint32_t now_ms, std::uint64_t* out, std::size_t max);
    // The ping pinged() booked never left the hub (the radio refused it): no offline for it. The backoff stays, so
    // the next ping comes a little later.
    void not_sent(std::uint64_t ieee);
    // A frame the device took neither at its address nor after the address was
    // looked up again, and nobody answered the lookup: as good as an unanswered
    // ping, so offline at the first take_offline after kActiveOfflineMs of
    // silence (once per silence). Pinged devices only, as for the ping.
    void unreachable(std::uint64_t ieee, std::uint32_t now_ms);
    bool offline(std::uint64_t ieee) const;

private:
    struct Slot {
        std::uint64_t ieee;         // 0 = free
        std::uint32_t due_ms;
        std::uint32_t ping_due_ms;
        std::uint32_t backoff_x2;   // ping backoff, doubled: 2 = 1x, 3 = 1.5x
        std::uint16_t nwk;
        std::uint8_t  flags;
        std::uint8_t  ping_ep;
        bool          awake;        // pinged when quiet
        bool          watched;      // availability on: offline verdicts at all
        bool          seen;
        bool          warned;       // already logged this failure streak
        bool          waiting;      // pinged, nothing heard since
        bool          offline;      // said so (take_offline); heard() clears it
        std::uint32_t ping_sent_ms; // the last ping
        std::uint32_t heard_ms;     // the last frame, or when the slot was made
    };
    Slot* find(std::uint64_t ieee);
    Slot slots_[kSlots] = {};
};

}  // namespace zhac_meter
