// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host-test shim implementations: FreeRTOS primitives (single-threaded)
// plus no-op stand-ins for the components simple_rules links against
// (rule_store, zigbee_pool, device_shadow, zhc_adapter, mqtt_gw,
// cron_parser). The rules engine + event_bus under test are the real code.
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "freertos/task.h"

#include "rule_store.h"
#include "zigbee_pool.h"
#include "device_shadow.h"
#include "zhc_adapter.h"
#include "mqtt_gw.h"
#include "cron_parser.h"

#include <cstring>
#include <cstdlib>

// ── Queue: fixed-capacity FIFO ring ──────────────────────────────────────

struct StubQueue {
    uint8_t*     buf;
    UBaseType_t  length;
    UBaseType_t  item_size;
    UBaseType_t  count;
    UBaseType_t  head;   // next slot to receive from
};

static unsigned long s_receive_budget = 0;   // 0 = unlimited
static unsigned long s_receive_used   = 0;
static bool          s_runaway        = false;

void stub_queue_set_receive_budget(unsigned long n) {
    s_receive_budget = n;
    s_receive_used   = 0;
    s_runaway        = false;
}

bool stub_queue_runaway(void) { return s_runaway; }

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size) {
    auto* q = static_cast<StubQueue*>(calloc(1, sizeof(StubQueue)));
    if (!q) return nullptr;
    q->buf = static_cast<uint8_t*>(calloc(length, item_size));
    if (!q->buf) { free(q); return nullptr; }
    q->length    = length;
    q->item_size = item_size;
    return q;
}

BaseType_t xQueueSend(QueueHandle_t q, const void* item, TickType_t ticks) {
    (void)ticks;
    if (!q || q->count == q->length) return pdFALSE;
    UBaseType_t tail = (q->head + q->count) % q->length;
    memcpy(q->buf + tail * q->item_size, item, q->item_size);
    q->count++;
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t q, void* item, TickType_t ticks) {
    (void)ticks;   // single-threaded: empty means empty, no waiting
    if (!q || q->count == 0) return pdFALSE;
    if (s_receive_budget != 0 && s_receive_used >= s_receive_budget) {
        // Queue still has events but the phase budget is spent — that is
        // the self-feeding wedge signature. Flag it and force the drain
        // loop to exit so the test can report instead of hanging.
        s_runaway = true;
        return pdFALSE;
    }
    s_receive_used++;
    memcpy(item, q->buf + q->head * q->item_size, q->item_size);
    q->head = (q->head + 1) % q->length;
    q->count--;
    return pdTRUE;
}

void vQueueDelete(QueueHandle_t q) {
    if (!q) return;
    free(q->buf);
    free(q);
}

// ── Semaphore: single-threaded, always succeeds ──────────────────────────

struct StubSemaphore { int depth; };

SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void) {
    return static_cast<SemaphoreHandle_t>(calloc(1, sizeof(StubSemaphore)));
}
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t s, TickType_t ticks) {
    (void)ticks;
    if (s) s->depth++;
    return pdTRUE;
}
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t s) {
    if (s && s->depth > 0) s->depth--;
    return pdTRUE;
}

// ── Timers: created but never fire ───────────────────────────────────────

struct StubTimer { void* id; };

TimerHandle_t xTimerCreate(const char* name, TickType_t period,
                           UBaseType_t auto_reload, void* timer_id,
                           TimerCallbackFunction_t cb) {
    (void)name; (void)period; (void)auto_reload; (void)cb;
    auto* t = static_cast<StubTimer*>(calloc(1, sizeof(StubTimer)));
    if (t) t->id = timer_id;
    return t;
}
BaseType_t xTimerStart(TimerHandle_t t, TickType_t ticks) { (void)t; (void)ticks; return pdTRUE; }
BaseType_t xTimerReset(TimerHandle_t t, TickType_t ticks) { (void)t; (void)ticks; return pdTRUE; }
BaseType_t xTimerChangePeriod(TimerHandle_t t, TickType_t period, TickType_t ticks) {
    (void)t; (void)period; (void)ticks; return pdTRUE;
}
void* pvTimerGetTimerID(TimerHandle_t t) { return t ? t->id : nullptr; }

// ── Tasks: recorded, never run (keeps task_cron out of the test) ─────────

BaseType_t xTaskCreate(TaskFunction_t fn, const char* name, uint32_t stack_depth,
                       void* arg, UBaseType_t priority, TaskHandle_t* out_handle) {
    (void)fn; (void)name; (void)stack_depth; (void)arg; (void)priority;
    if (out_handle) *out_handle = nullptr;
    return pdPASS;
}
void vTaskDelay(TickType_t ticks) { (void)ticks; }

// ── rule_store: empty store, writes are dropped ──────────────────────────

bool     rule_store_load(uint16_t rule_id, RuleSlot* out) { (void)rule_id; (void)out; return false; }
uint16_t rule_store_load_all(RuleSlot* out, uint16_t max_count) { (void)out; (void)max_count; return 0; }
void     rule_store_mark_dirty(const RuleSlot* slot) { (void)slot; }
void     rule_store_mark_delete(uint16_t rule_id) { (void)rule_id; }

// ── zigbee_pool: empty pool ──────────────────────────────────────────────

void       zigbee_pool_lock() {}
void       zigbee_pool_unlock() {}
ZapDevice* pool_find_by_ieee(uint64_t ieee) { (void)ieee; return nullptr; }
ZapDevice* pool_all() { return nullptr; }
uint16_t   pool_count() { return 0; }

// ── device_shadow: no cached attributes ──────────────────────────────────

uint8_t device_shadow_get_attrs(uint64_t ieee, ShadowAttr* out, uint8_t max_count) {
    (void)ieee; (void)out; (void)max_count;
    return 0;
}

// ── zhc_adapter / mqtt_gw: sinks ─────────────────────────────────────────

bool zhac_adapter_send_uint(uint64_t ieee, const char* model_id,
                            const char* manufacturer_name,
                            uint16_t nwk_addr, uint8_t dst_endpoint,
                            const char* key, uint64_t value) {
    (void)ieee; (void)model_id; (void)manufacturer_name;
    (void)nwk_addr; (void)dst_endpoint; (void)key; (void)value;
    return true;
}

void mqtt_gw_publish(const char* topic, const char* payload, size_t payload_len,
                     int qos, bool retain) {
    (void)topic; (void)payload; (void)payload_len; (void)qos; (void)retain;
}

// ── cron_parser: nothing ever matches ────────────────────────────────────

bool cron_parse(const char* expr, CronExpr& out) { (void)expr; (void)out; return false; }
bool cron_matches(const CronExpr& expr, time_t t) { (void)expr; (void)t; return false; }
