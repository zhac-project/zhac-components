#pragma once
#include "esp_err.h"
#include "esp_log.h"
static inline esp_err_t nvs_seq(esp_err_t* acc, esp_err_t r, const char* tag, const char* op) {
    if (r != ESP_OK) { ESP_LOGE(tag, "nvs %s: %s", op, esp_err_to_name(r)); if (acc && *acc == ESP_OK) *acc = r; }
    return r;
}
