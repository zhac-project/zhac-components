// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// mqtt_payload.cpp -- see mqtt_payload.h. One writer (the MQTT client's task)
// and any readers; a mutex, as each side copies at most 1 KB under it.
#include "mqtt_payload.h"

#include <cstring>
#include <mutex>

#include "esp_heap_caps.h"

namespace {

constexpr size_t kInline = sizeof(MqttMsgEvent::payload) - 1;   // 31: the message itself
constexpr size_t kPrefix = sizeof(MqttMsgEvent::payload) - 5;   // 27, then a NUL and the id
constexpr int    kSlots  = 16;

struct Slot {
    uint32_t id;
    uint16_t len;
    char     text[kMqttPayloadMax];
};
Slot*      s_ring = nullptr;   // PSRAM, from the first long message on
uint32_t   s_last = 0;         // the last id given; 0 = none
std::mutex s_mu;

}  // namespace

void mqtt_msg_set_payload(MqttMsgEvent& m, const char* data, size_t len) {
    std::memset(m.payload, 0, sizeof(m.payload));
    if (len <= kInline) {
        if (len) std::memcpy(m.payload, data, len);
        // Bytes after a NUL at 27 are no consumer's text; zero them, so they never read as an id.
        if (m.payload[kPrefix] == '\0') std::memset(m.payload + kPrefix, 0, sizeof(m.payload) - kPrefix);
        return;
    }
    std::memcpy(m.payload, data, kPrefix);
    std::lock_guard<std::mutex> lock(s_mu);
    if (!s_ring) s_ring = static_cast<Slot*>(heap_caps_calloc(kSlots, sizeof(Slot), MALLOC_CAP_SPIRAM));
    if (!s_ring) return;   // no memory: the 27 characters
    if (++s_last == 0) s_last = 1;
    Slot& s = s_ring[s_last % kSlots];
    s.id  = s_last;
    s.len = static_cast<uint16_t>(len < kMqttPayloadMax ? len : kMqttPayloadMax);
    std::memcpy(s.text, data, s.len);
    std::memcpy(m.payload + kPrefix + 1, &s_last, sizeof(s_last));
}

size_t mqtt_msg_full_payload(const MqttMsgEvent& m, char* out, size_t cap) {
    uint32_t id;
    std::memcpy(&id, m.payload + kPrefix + 1, sizeof(id));
    if (m.payload[kPrefix] != '\0' || id == 0) return 0;   // the event holds it all
    std::lock_guard<std::mutex> lock(s_mu);
    if (!s_ring) return 0;
    const Slot& s = s_ring[id % kSlots];
    if (s.id != id || s.len > cap) return 0;
    std::memcpy(out, s.text, s.len);
    return s.len;
}
