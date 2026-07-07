#pragma once
#include <cstdio>
#include <cstdlib>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG   0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NVS_BASE 0x1100
#define ESP_ERR_NVS_NOT_FOUND         (ESP_ERR_NVS_BASE + 0x02)
#define ESP_ERR_NVS_INVALID_LENGTH    (ESP_ERR_NVS_BASE + 0x04)
#define ESP_ERR_NVS_NO_FREE_PAGES     (ESP_ERR_NVS_BASE + 0x0d)
#define ESP_ERR_NVS_NEW_VERSION_FOUND (ESP_ERR_NVS_BASE + 0x10)
static inline const char* esp_err_to_name(esp_err_t e) { (void)e; return "ESP_ERR"; }
// Host shim for ESP_ERROR_CHECK: abort on a non-OK error (the in-memory NVS
// stub always returns ESP_OK for init/erase, so this never fires in tests).
#define ESP_ERROR_CHECK(x) do {                                              \
    esp_err_t _rc_ = (x);                                                    \
    if (_rc_ != ESP_OK) {                                                    \
        fprintf(stderr, "ESP_ERROR_CHECK failed rc=%d\n", _rc_);            \
        abort();                                                             \
    }                                                                        \
} while (0)
