#pragma once
#include <cstdio>

// Runtime log level enum + level-parameterised macro. zcl_commands.cpp stores
// an esp_log_level_t in AfReqOpts and emits via ESP_LOG_LEVEL(level, ...).
// Host shim: the level is consumed (so it never looks unused) but nothing is
// printed, matching the silent ESP_LOGI/D/V below.
typedef enum {
    ESP_LOG_NONE = 0,
    ESP_LOG_ERROR,
    ESP_LOG_WARN,
    ESP_LOG_INFO,
    ESP_LOG_DEBUG,
    ESP_LOG_VERBOSE,
} esp_log_level_t;
#define ESP_LOG_LEVEL(level, tag, fmt, ...) ((void)(level))

#define ESP_LOGE(tag, fmt, ...) fprintf(stderr, "E (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) fprintf(stderr, "W (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) ((void)0)
#define ESP_LOGD(tag, fmt, ...) ((void)0)
#define ESP_LOGV(tag, fmt, ...) ((void)0)
