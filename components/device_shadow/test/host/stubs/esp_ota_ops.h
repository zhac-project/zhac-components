#pragma once
#include "esp_err.h"
#include "esp_partition.h"
typedef enum {
    ESP_OTA_IMG_NEW = 0x0, ESP_OTA_IMG_PENDING_VERIFY = 0x1, ESP_OTA_IMG_VALID = 0x2,
    ESP_OTA_IMG_INVALID = 0x3, ESP_OTA_IMG_ABORTED = 0x4, ESP_OTA_IMG_UNDEFINED = -1
} esp_ota_img_states_t;
const esp_partition_t* esp_ota_get_running_partition(void);
esp_err_t esp_ota_get_state_partition(const esp_partition_t*, esp_ota_img_states_t*);
