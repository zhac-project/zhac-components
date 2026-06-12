// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: Apache-2.0
// Unity on-target tests for the metrics engine. Each TEST_CASE calls
// metrics::init() first to zero shard storage — tests share global
// state and must not depend on preceding cases.

#include "unity.h"
#include "metrics/metrics.h"
#include "metrics/metrics_export_prometheus.h"
#include "metrics/metrics_export_mqtt.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <atomic>
#include <cstdint>
#include <cstring>

using metrics::MetricId;

namespace {
// Short aliases so lines don't wrap in TEST_ASSERTs.
constexpr auto kHapFrames = MetricId::METRIC_HAP_RX_FRAMES_TOTAL;
constexpr auto kHapTimer  = MetricId::METRIC_HAP_RX_HANDLE;
constexpr auto kHeapFree  = MetricId::METRIC_HEAP_FREE_BYTES;

void reset() { metrics::init(); }
}  // namespace

// ── Counter ──────────────────────────────────────────────────────────

TEST_CASE("counter — inc accumulates", "[metrics]") {
    reset();
    metrics::counter_inc(kHapFrames, 5);
    metrics::counter_inc(kHapFrames, 3);
    metrics::CounterSnapshot s{};
    TEST_ASSERT_TRUE(metrics::read_counter(kHapFrames, s));
    TEST_ASSERT_EQUAL_UINT64(8, s.value);
}

TEST_CASE("counter — set replaces aggregate", "[metrics]") {
    reset();
    metrics::counter_inc(kHapFrames, 100);
    metrics::counter_set(kHapFrames, 42);
    metrics::CounterSnapshot s{};
    metrics::read_counter(kHapFrames, s);
    TEST_ASSERT_EQUAL_UINT64(42, s.value);
}

TEST_CASE("counter — wrong kind returns false", "[metrics]") {
    reset();
    metrics::CounterSnapshot s{};
    // METRIC_HEAP_FREE_BYTES is a value, not a counter.
    TEST_ASSERT_FALSE(metrics::read_counter(kHeapFree, s));
}

// ── Value ────────────────────────────────────────────────────────────

TEST_CASE("value — min/max/avg/count", "[metrics]") {
    reset();
    metrics::value_record(kHeapFree, 100);
    metrics::value_record(kHeapFree, 50);
    metrics::value_record(kHeapFree, 200);
    metrics::ValueSnapshot s{};
    TEST_ASSERT_TRUE(metrics::read_value(kHeapFree, s));
    TEST_ASSERT_TRUE(s.has_data);
    TEST_ASSERT_EQUAL_UINT64(3, s.count);
    TEST_ASSERT_EQUAL_INT64(50,  s.min);
    TEST_ASSERT_EQUAL_INT64(200, s.max);
    TEST_ASSERT_EQUAL_INT64(350, s.sum);
    TEST_ASSERT_EQUAL_INT64(116, s.avg);   // 350/3 with integer truncation
    TEST_ASSERT_EQUAL_INT64(200, s.last);
}

TEST_CASE("value — no samples has_data=false", "[metrics]") {
    reset();
    metrics::ValueSnapshot s{};
    TEST_ASSERT_TRUE(metrics::read_value(kHeapFree, s));   // kind match
    TEST_ASSERT_FALSE(s.has_data);
    TEST_ASSERT_EQUAL_UINT64(0, s.count);
}

// ── Timer ────────────────────────────────────────────────────────────

TEST_CASE("timer — record direct", "[metrics]") {
    reset();
    metrics::timer_record(kHapTimer, 42);
    metrics::timer_record(kHapTimer, 100);
    metrics::TimerSnapshot s{};
    metrics::read_timer(kHapTimer, s);
    TEST_ASSERT_TRUE(s.has_data);
    TEST_ASSERT_EQUAL_UINT64(2, s.count);
    TEST_ASSERT_EQUAL_UINT32(42,  s.min_us);
    TEST_ASSERT_EQUAL_UINT32(100, s.max_us);
    TEST_ASSERT_EQUAL_UINT32(71,  s.avg_us);
}

TEST_CASE("timer — scope measures > 0", "[metrics]") {
    reset();
    {
        metrics::TimerScope _scope(kHapTimer);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    metrics::TimerSnapshot s{};
    metrics::read_timer(kHapTimer, s);
    TEST_ASSERT_TRUE(s.has_data);
    TEST_ASSERT_EQUAL_UINT64(1, s.count);
    // Generous window — scheduler jitter and tick alignment move this.
    TEST_ASSERT_GREATER_THAN_UINT32(5000,  s.last_us);
    TEST_ASSERT_LESS_THAN_UINT32(100000,   s.last_us);
}

TEST_CASE("timer — double stop records once", "[metrics]") {
    reset();
    auto tok = metrics::timer_start(kHapTimer);
    metrics::timer_stop(tok);
    metrics::timer_stop(tok);   // must be a no-op
    metrics::TimerSnapshot s{};
    metrics::read_timer(kHapTimer, s);
    TEST_ASSERT_EQUAL_UINT64(1, s.count);
}

TEST_CASE("timer — nested scopes independent", "[metrics]") {
    reset();
    {
        metrics::TimerScope outer(kHapTimer);
        {
            metrics::TimerScope inner(kHapTimer);
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    metrics::TimerSnapshot s{};
    metrics::read_timer(kHapTimer, s);
    TEST_ASSERT_EQUAL_UINT64(2, s.count);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(s.min_us, s.max_us - s.max_us); // sanity
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(s.min_us, 1000);
}

// ── Prometheus exporter ──────────────────────────────────────────────

TEST_CASE("prometheus — contains counter and timer lines", "[metrics]") {
    reset();
    metrics::counter_inc(kHapFrames, 7);
    metrics::timer_record(kHapTimer, 500);
    char buf[2048];
    const size_t n = metrics::prometheus_format(buf, sizeof(buf), "test");
    TEST_ASSERT_GREATER_THAN_UINT(0, n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "test_hap_rx_frames_total 7"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "test_hap_rx_handle_us_count 1"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "test_hap_rx_handle_us_sum 500"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "test_hap_rx_handle_us_min 500"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "test_hap_rx_handle_us_max 500"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "# TYPE test_hap_rx_frames_total counter"));
}

TEST_CASE("prometheus — truncation-safe NUL-terminates", "[metrics]") {
    reset();
    metrics::counter_inc(kHapFrames, 1);
    char tiny[32];
    std::memset(tiny, 0xAB, sizeof(tiny));
    const size_t n = metrics::prometheus_format(tiny, sizeof(tiny), "zhac");
    TEST_ASSERT_LESS_OR_EQUAL_UINT(sizeof(tiny) - 1, n);
    TEST_ASSERT_EQUAL_CHAR('\0', tiny[sizeof(tiny) - 1]);
}

TEST_CASE("prometheus — zero-size buffer", "[metrics]") {
    reset();
    TEST_ASSERT_EQUAL_UINT(0, metrics::prometheus_format(nullptr, 0, "x"));
}

// ── MQTT JSON exporter ───────────────────────────────────────────────

TEST_CASE("mqtt json — has top-level sections", "[metrics]") {
    reset();
    metrics::counter_inc(kHapFrames, 42);
    metrics::value_record(kHeapFree, 12345);
    char buf[2048];
    const size_t n = metrics::mqtt_format_snapshot_json(buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN_UINT(0, n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"timers\":{"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"counters\":{"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"values\":{"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"hap_rx_frames_total\":42"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"heap_free_bytes\":{"));
    TEST_ASSERT_EQUAL_CHAR('}', buf[n - 1]);
}

TEST_CASE("mqtt json — truncation returns 0 (no garbage publish)",
          "[metrics]") {
    reset();
    metrics::counter_inc(kHapFrames, 1);
    char tiny[24];
    std::memset(tiny, 0xAB, sizeof(tiny));
    // A buffer too small to hold the closing braces yields invalid JSON.
    // The exporter must report 0 so the caller skips the broker publish
    // (FINDINGS §8) — and still leave the buffer NUL-terminated.
    const size_t n = metrics::mqtt_format_snapshot_json(tiny, sizeof(tiny));
    TEST_ASSERT_EQUAL_UINT(0, n);
    TEST_ASSERT_EQUAL_CHAR('\0', tiny[sizeof(tiny) - 1]);
}

TEST_CASE("mqtt json — untruncated returns >0 and closes braces",
          "[metrics]") {
    reset();
    metrics::counter_inc(kHapFrames, 7);
    char buf[2048];
    const size_t n = metrics::mqtt_format_snapshot_json(buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN_UINT(0, n);
    TEST_ASSERT_EQUAL_CHAR('}', buf[n - 1]);   // valid: closed object
}

// ── Text dump ────────────────────────────────────────────────────────

TEST_CASE("dump_text — contains metric names", "[metrics]") {
    reset();
    metrics::counter_inc(kHapFrames, 1);
    char buf[512];
    const size_t n = metrics::dump_text(buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN_UINT(0, n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "hap_rx_frames_total => 1"));
}

// ── Memory snapshot ──────────────────────────────────────────────────

TEST_CASE("memory — snapshot populates heap metrics", "[metrics]") {
    reset();
    metrics::update_memory_snapshot();
    metrics::ValueSnapshot s{};
    metrics::read_value(kHeapFree, s);
    TEST_ASSERT_TRUE(s.has_data);
    TEST_ASSERT_GREATER_THAN_INT64(0, s.last);
}

// ── Concurrency ──────────────────────────────────────────────────────

namespace {

struct HammerCtx {
    int                count;
    std::atomic<bool>  done;
};

void hammer_task(void* arg) {
    auto* ctx = static_cast<HammerCtx*>(arg);
    for (int i = 0; i < ctx->count; ++i) {
        metrics::counter_inc(kHapFrames, 1);
    }
    ctx->done.store(true, std::memory_order_release);
    vTaskDelete(nullptr);
}

}  // namespace

TEST_CASE("concurrency — two tasks pin to opposite cores", "[metrics]") {
    reset();
    constexpr int kPerTask = 5000;

    HammerCtx c0{kPerTask, {false}};
    HammerCtx c1{kPerTask, {false}};

    xTaskCreatePinnedToCore(hammer_task, "ham0", 4096, &c0, 1, nullptr, 0);
    xTaskCreatePinnedToCore(hammer_task, "ham1", 4096, &c1, 1, nullptr, 1);

    // Wait up to 5 s for both tasks to finish.
    for (int i = 0; i < 500; ++i) {
        if (c0.done.load(std::memory_order_acquire) &&
            c1.done.load(std::memory_order_acquire)) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    TEST_ASSERT_TRUE(c0.done.load());
    TEST_ASSERT_TRUE(c1.done.load());

    metrics::CounterSnapshot s{};
    metrics::read_counter(kHapFrames, s);
    TEST_ASSERT_EQUAL_UINT64(2ULL * kPerTask, s.value);
}
