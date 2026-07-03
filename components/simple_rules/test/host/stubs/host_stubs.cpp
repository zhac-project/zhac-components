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

static bool s_fail_next_create = false;

void stub_queue_fail_next_create(void) { s_fail_next_create = true; }

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size) {
    if (s_fail_next_create) { s_fail_next_create = false; return nullptr; }
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

// ── rule_store: in-memory store model (P2-T18) ───────────────────────────
//
// The default behaviour is an empty store (matching the original stub). The
// id/capacity tests seed a fake persisted store via stub_rule_store_seed_id()
// so they can prove next_rule_id() derives from the WHOLE store, not just the
// simple_rules 64-entry cache.
static uint16_t s_store_ids[512];
static uint16_t s_store_n = 0;

void stub_rule_store_reset(void) { s_store_n = 0; }
void stub_rule_store_seed_id(uint16_t id) {
    if (s_store_n < (uint16_t)(sizeof(s_store_ids) / sizeof(s_store_ids[0])))
        s_store_ids[s_store_n++] = id;
}
uint16_t stub_rule_store_count(void) { return s_store_n; }

bool rule_store_load(uint16_t rule_id, RuleSlot* out) {
    for (uint16_t i = 0; i < s_store_n; i++) {
        if (s_store_ids[i] == rule_id) {
            if (out) { memset(out, 0, sizeof(*out)); out->rule_id = rule_id; }
            return true;
        }
    }
    return false;
}
uint16_t rule_store_load_all(RuleSlot* out, uint16_t max_count) { (void)out; (void)max_count; return 0; }
void     rule_store_mark_dirty(const RuleSlot* slot) {
    // Record the create so a subsequent next_rule_id() sees it persisted,
    // mirroring the real writeback overlay being folded into rule_store_max_id.
    if (slot && !rule_store_load(slot->rule_id, nullptr)) stub_rule_store_seed_id(slot->rule_id);
}
void     rule_store_mark_delete(uint16_t rule_id) {
    for (uint16_t i = 0; i < s_store_n; i++) {
        if (s_store_ids[i] == rule_id) {
            s_store_ids[i] = s_store_ids[--s_store_n];
            return;
        }
    }
}
uint16_t rule_store_max_id(void) {
    uint16_t m = 0;
    for (uint16_t i = 0; i < s_store_n; i++) if (s_store_ids[i] > m) m = s_store_ids[i];
    return m;
}
uint16_t rule_store_count(void) { return s_store_n; }

// ── zigbee_pool: seedable single-device pool (default empty) ──────────────
// Existing tests keep the empty behaviour; test_action_shadow seeds one
// device so a rule `zigbee.set` action can resolve its target device.
static ZapDevice s_pool[1];
static uint16_t  s_pool_n = 0;

void stub_pool_reset(void)              { s_pool_n = 0; }
void stub_pool_seed(const ZapDevice* d) { if (d) { s_pool[0] = *d; s_pool_n = 1; } }

void       zigbee_pool_lock() {}
void       zigbee_pool_unlock() {}
ZapDevice* pool_find_by_ieee(uint64_t ieee) {
    return (s_pool_n && s_pool[0].ieee_addr == ieee) ? &s_pool[0] : nullptr;
}
ZapDevice* pool_all()   { return s_pool_n ? s_pool : nullptr; }
uint16_t   pool_count() { return s_pool_n; }

// ── device_shadow: no cached attributes; records optimistic writes ────────

uint8_t device_shadow_get_attrs(uint64_t ieee, ShadowAttr* out, uint8_t max_count) {
    (void)ieee; (void)out; (void)max_count;
    return 0;
}

bool device_shadow_get_attr(uint64_t ieee, const char* key, ShadowAttr* out) {
    (void)ieee; (void)key; (void)out;
    return false;
}

// Recorder for the optimistic write the rule engine performs after a
// successful zigbee.set (mirrors the webui SET_ATTRIBUTE path).
static int      s_opt_count = 0;
static uint64_t s_opt_ieee  = 0;
static char     s_opt_key[32] = {0};
static uint8_t  s_opt_vt    = 0;
static int32_t  s_opt_val   = 0;

void        stub_shadow_opt_reset(void) { s_opt_count = 0; s_opt_ieee = 0;
                                          s_opt_key[0] = '\0'; s_opt_vt = 0; s_opt_val = 0; }
int         stub_shadow_opt_count(void) { return s_opt_count; }
uint64_t    stub_shadow_opt_ieee(void)  { return s_opt_ieee; }
const char* stub_shadow_opt_key(void)   { return s_opt_key; }
uint8_t     stub_shadow_opt_vt(void)    { return s_opt_vt; }
int32_t     stub_shadow_opt_val(void)   { return s_opt_val; }

void device_shadow_update_optimistic(uint64_t ieee, const char* key,
                                     uint8_t val_type, int32_t int_val) {
    s_opt_count++;
    s_opt_ieee = ieee;
    strncpy(s_opt_key, key ? key : "", sizeof(s_opt_key) - 1);
    s_opt_key[sizeof(s_opt_key) - 1] = '\0';
    s_opt_vt  = val_type;
    s_opt_val = int_val;
}

// ── zhc_adapter / mqtt_gw: sinks ─────────────────────────────────────────
// Send result is settable so a test can exercise the failure path (no
// optimistic shadow write when the command never went out).
static bool s_send_result = true;
void stub_adapter_send_set_result(bool r) { s_send_result = r; }

bool zhac_adapter_send_uint(uint64_t ieee, const char* model_id,
                            const char* manufacturer_name,
                            uint16_t nwk_addr, uint8_t dst_endpoint,
                            const char* key, uint64_t value) {
    (void)ieee; (void)model_id; (void)manufacturer_name;
    (void)nwk_addr; (void)dst_endpoint; (void)key; (void)value;
    return s_send_result;
}

void mqtt_gw_publish(const char* topic, const char* payload, size_t payload_len,
                     int qos, bool retain) {
    (void)topic; (void)payload; (void)payload_len; (void)qos; (void)retain;
}

// ── cron_parser: nothing ever matches ────────────────────────────────────

bool cron_parse(const char* expr, CronExpr& out) { (void)expr; (void)out; return false; }
bool cron_matches(const CronExpr& expr, time_t t) { (void)expr; (void)t; return false; }
