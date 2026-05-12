// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>

// Protocol type tag — distinguishes device transport layer.
// PROTO_ZIGBEE is 0 so that zero-initialized structs default to Zigbee.
typedef enum : uint8_t {
    PROTO_ZIGBEE  = 0x00,
    PROTO_BLE     = 0x01,
    PROTO_THREAD  = 0x02,
    PROTO_WIFI    = 0x03,
    PROTO_ZWAVE   = 0x04,
} NcpProtocol;

// Short protocol name for JSON serialization (e.g. "zb", "ble", "th")
inline const char* ncp_protocol_name(NcpProtocol p) {
    switch (p) {
        case PROTO_ZIGBEE: return "zb";
        case PROTO_BLE:    return "ble";
        case PROTO_THREAD: return "th";
        case PROTO_WIFI:   return "wifi";
        case PROTO_ZWAVE:  return "zw";
        default:           return "?";
    }
}

// Snapshot partition header (64 bytes)
// Layout: magic(4) + sequence(4) + device_count(2) + attr_count(2) + crc32(4) + _pad(48) = 64
struct __attribute__((packed)) SnapHeader {
    uint32_t magic;         // 0x5A415053 'ZAPS'
    uint32_t sequence;      // monotonic, compared A vs B on boot
    uint16_t device_count;
    uint16_t attr_count;
    uint32_t crc32;         // CRC32 of entire partition
    uint8_t  _pad[48];
};
static_assert(sizeof(SnapHeader) == 64);

// ── Lifecycle state enums (backend-agnostic) ─────────────────────────────
// Each state is explicitly persisted in ZapDevice so a device's position
// in the join → interview → match → configure pipeline survives reboot.

// Interview progress for a device. NONE = never interviewed; TOPOLOGY_READY
// = node+endpoints known but Basic cluster missing/incomplete; IDENTITY_
// PENDING = waiting on late Basic traffic; IDENTITY_READY = model+mfg known;
// FAILED = topology probe itself failed (no endpoints).
enum class InterviewState : uint8_t {
    NONE             = 0,
    TOPOLOGY_READY   = 1,
    IDENTITY_PENDING = 2,
    IDENTITY_READY   = 3,
    FAILED           = 4,
};

// Converter-match outcome.
enum class SupportState : uint8_t {
    UNKNOWN   = 0,
    UNMATCHED = 1,
    MATCHED   = 2,
    PARTIAL   = 3,
};

// Post-match configure status (reporting, binding, etc.).
enum class ConfigureState : uint8_t {
    PENDING     = 0,
    IN_PROGRESS = 1,
    DONE        = 2,
    FAILED      = 3,
};

// Fixed device record (522 bytes, packed)
// Layout: ieee_addr(8)+nwk_addr(2)+endpoint_count(1)+device_type(1)+protocol(1)+_proto_pad(1)+
//   last_seen(4)+manufacturer_code(2)+model_id(34)+manufacturer_name(34)+friendly_name(30)+
//   endpoints(8)+clusters_in(8*12*2=192)+clusters_out(8*12*2=192)+
//   link_quality(1)+power_source(1)+battery_pct(2)+
//   interview_state(1)+support_state(1)+configure_state(1)+configure_attempts(1)+crc32(4) = 522 bytes
// ZAP_CLUSTERS_PER_EP=12 covers observed max of 10 in + 8 out clusters per endpoint.
// clusters_in  = server-side clusters (what the device receives / exposes attributes)
// clusters_out = client-side clusters (what the device sends — ZDO bind source)
static constexpr uint8_t ZAP_CLUSTERS_PER_EP = 12;

struct ZapDevice {
    uint64_t ieee_addr;
    union {
        uint16_t nwk_addr;      // Zigbee: 16-bit short address
        uint16_t conn_handle;   // BLE: connection handle
        uint16_t rloc16;        // Thread: RLOC16
        uint16_t addr_raw;      // generic access
    };
    uint8_t      endpoint_count;
    uint8_t      device_type;
    NcpProtocol  protocol;      // PROTO_ZIGBEE, PROTO_BLE, etc.
    uint8_t      flags;         // bit0 = ZAP_DEV_REMOVED (soft-removed,
                                // hidden from user UI). Was `_proto_pad`
                                // padding — repurposed 2026-04-19.
    uint32_t last_seen;
    uint16_t manufacturer_code;
    char     model_id[34];           // Basic cluster attr 0x0005 (modelIdentifier)
    char     manufacturer_name[34];  // Basic cluster attr 0x0004 (manufacturerName)
    char     friendly_name[30];
    uint8_t  endpoints[8];
    uint16_t clusters[8][ZAP_CLUSTERS_PER_EP];      // in-clusters (server side)
    uint16_t clusters_out[8][ZAP_CLUSTERS_PER_EP];  // out-clusters (client side, ZDO bind src)
    uint8_t  link_quality;
    uint8_t  power_source;
    uint16_t battery_pct;
    uint8_t  interview_state;      // InterviewState enum
    uint8_t  support_state;        // SupportState enum
    uint8_t  configure_state;      // ConfigureState enum
    uint8_t  configure_attempts;   // retry counter for deferred configure
    uint32_t crc32;
} __attribute__((packed));
static_assert(sizeof(ZapDevice) == 522);

// ZapDevice.flags bits.
static constexpr uint8_t ZAP_DEV_REMOVED = 1 << 0;  // soft-removed / hidden

static inline bool zap_dev_is_removed(const ZapDevice* d) {
    return d && (d->flags & ZAP_DEV_REMOVED);
}
static inline void zap_dev_mark_removed(ZapDevice* d) {
    if (d) d->flags |= ZAP_DEV_REMOVED;
}
static inline void zap_dev_clear_removed(ZapDevice* d) {
    if (d) d->flags &= static_cast<uint8_t>(~ZAP_DEV_REMOVED);
}

// Journal entry header (12 bytes, payload follows in flash)
struct __attribute__((packed)) JournalEntry {
    uint8_t  type;          // ADD_DEVICE=1 UPDATE_ATTR=2 REMOVE=3 RENAME=4
    uint8_t  _pad;
    uint16_t payload_len;
    uint32_t seq;
    uint32_t crc32;
};
static_assert(sizeof(JournalEntry) == 12);

// rule_type values
enum class RuleType : uint8_t {
    SIMPLE = 0,
    // 1 was BERRY — Berry engine was removed 2026-04-21. Reserved so
    // legacy rule slots read from NVS with rule_type=1 parse without
    // conflict; a post-load migration step downgrades them to SIMPLE
    // or drops them.
    LEGACY_RESERVED_1 = 1,
};

enum class TriggerType : uint8_t {
    DEVICE_ATTR = 1,
    TIME_CRON   = 2,
    EVENT       = 3,
    BOOT        = 4,
    TIMER       = 5,
    MQTT_TOPIC  = 6,
};

// Rule slot (536 bytes; bumped 2026-04-22 to add `name`).
// Layout: rule_id(2)+enabled(1)+_reserved(1)+src_len(2)+trigger_type(1)+
//         rule_type(1)+name(24)+src(500)+crc32(4) = 536.
//
// Prior-version (512-byte) rows loaded from NVS fail length validation
// and are discarded. Early-dev stance allows the breakage; operators
// re-save their rules on upgrade.
struct __attribute__((packed)) RuleSlot {
    uint16_t rule_id;
    uint8_t  enabled;
    uint8_t  _reserved;      // was 'version' — never set/checked; kept for layout stability
    uint16_t src_len;        // DSL text length
    uint8_t  trigger_type;   // TriggerType enum value
    uint8_t  rule_type;      // RuleType enum value (was _pad)
    char     name[24];       // friendly display name, NUL-terminated
    uint8_t  src[500];       // DSL text
    uint32_t crc32;
};
static_assert(sizeof(RuleSlot) == 536);

static constexpr uint16_t ZAP_MAX_DEVICES = 200;
static constexpr uint16_t ZAP_MAX_RULES   = 256;
