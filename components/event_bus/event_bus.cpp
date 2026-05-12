// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// components/event_bus/event_bus.cpp
#include "event_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_log.h"

static const char* TAG = "event_bus";

static constexpr uint8_t MAX_SUBS_PER_TYPE = 8;
static constexpr uint8_t EVENT_TYPE_COUNT  = 11;
static constexpr uint8_t QUEUE_DEPTH       = 16;

struct SubEntry {
    EventHandler  handler;
    QueueHandle_t queue;
    EventFilter   filter;   // optional — null means accept all events
};

static SubEntry s_subs[EVENT_TYPE_COUNT][MAX_SUBS_PER_TYPE];
// s_sub_hwm[t] = highest used slot index + 1 for type t
static uint8_t  s_sub_hwm[EVENT_TYPE_COUNT];

void event_bus_init() {
    for (uint8_t t = 0; t < EVENT_TYPE_COUNT; t++) {
        s_sub_hwm[t] = 0;
        for (uint8_t i = 0; i < MAX_SUBS_PER_TYPE; i++) {
            s_subs[t][i].handler = nullptr;
            s_subs[t][i].queue   = nullptr;
        }
    }
    ESP_LOGI(TAG, "event_bus init OK");
}

EventSubHandle event_bus_subscribe(EventType type, EventHandler handler,
                                   EventFilter filter) {
    uint8_t idx = static_cast<uint8_t>(type);
    if (idx == 0 || idx >= EVENT_TYPE_COUNT) {
        ESP_LOGE(TAG, "invalid event type %d", idx);
        return EVENT_SUB_INVALID;
    }

    // Find first free slot (supports re-use after unsubscribe)
    int8_t pos = -1;
    for (uint8_t i = 0; i < MAX_SUBS_PER_TYPE; i++) {
        if (s_subs[idx][i].queue == nullptr && !s_subs[idx][i].handler) {
            pos = (int8_t)i;
            break;
        }
    }
    if (pos < 0) {
        ESP_LOGE(TAG, "subscriber table full for type %d", idx);
        return EVENT_SUB_INVALID;
    }

    QueueHandle_t q = xQueueCreate(QUEUE_DEPTH, sizeof(Event));
    configASSERT(q);
    s_subs[idx][pos] = {handler, q, filter};
    if ((uint8_t)(pos + 1) > s_sub_hwm[idx])
        s_sub_hwm[idx] = (uint8_t)(pos + 1);

    ESP_LOGI(TAG, "subscribed type=%d pos=%d", idx, pos);
    return (EventSubHandle)((idx << 8) | (uint8_t)pos);
}

void event_bus_unsubscribe(EventSubHandle handle) {
    if (handle == EVENT_SUB_INVALID) return;
    uint8_t idx = (handle >> 8) & 0xFF;
    uint8_t pos = handle & 0xFF;
    if (idx == 0 || idx >= EVENT_TYPE_COUNT || pos >= MAX_SUBS_PER_TYPE) return;

    if (s_subs[idx][pos].queue) {
        vQueueDelete(s_subs[idx][pos].queue);
        s_subs[idx][pos].queue = nullptr;
    }
    s_subs[idx][pos].handler = nullptr;

    // Recalculate hwm
    while (s_sub_hwm[idx] > 0 &&
           s_subs[idx][s_sub_hwm[idx] - 1].queue == nullptr &&
           !s_subs[idx][s_sub_hwm[idx] - 1].handler)
        s_sub_hwm[idx]--;

    ESP_LOGI(TAG, "unsubscribed type=%d pos=%d", idx, pos);
}

void event_bus_publish(const Event& e) {
    uint8_t idx = static_cast<uint8_t>(e.type);
    if (idx == 0 || idx >= EVENT_TYPE_COUNT) return;

    for (uint8_t i = 0; i < s_sub_hwm[idx]; i++) {
        SubEntry& sub = s_subs[idx][i];
        if (!sub.queue && !sub.handler) continue;   // unsubscribed slot
        if (sub.filter && !sub.filter(e)) continue; // predicate filtered out

        if (sub.queue) {
            if (xQueueSend(sub.queue, &e, 0) != pdTRUE) {
                // Eviction policy: overwrite-oldest (E3).
                // On queue full, the oldest event is silently discarded to make room
                // for the new one. This is intentional: for high-rate sensor bursts
                // (ZCL attribute updates, MQTT messages) the newest value is always
                // more relevant than a stale reading. Subscribers that cannot keep up
                // will see the latest state rather than a backlog of outdated events.
                Event discard{};
                xQueueReceive(sub.queue, &discard, 0);
                xQueueSend(sub.queue, &e, 0);
                ESP_LOGW(TAG, "queue full type=%d sub=%d — oldest overwritten", idx, i);
            }
        } else if (sub.handler) {
            sub.handler(e);
        }
    }
}

uint8_t event_bus_drain(EventType type, uint32_t timeout_ms) {
    uint8_t idx = static_cast<uint8_t>(type);
    if (idx == 0 || idx >= EVENT_TYPE_COUNT || s_sub_hwm[idx] == 0) return 0;

    uint8_t count = 0;
    Event ev{};
    for (uint8_t i = 0; i < s_sub_hwm[idx]; i++) {
        QueueHandle_t q = s_subs[idx][i].queue;
        if (!q) continue;
        TickType_t ticks = (count == 0) ? pdMS_TO_TICKS(timeout_ms) : 0;
        while (xQueueReceive(q, &ev, ticks) == pdTRUE) {
            if (s_subs[idx][i].handler) s_subs[idx][i].handler(ev);
            count++;
            ticks = 0;
        }
    }
    return count;
}
