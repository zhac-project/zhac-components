#pragma once
#include <cstdint>
typedef enum { ESP_PARTITION_TYPE_APP = 0x00, ESP_PARTITION_TYPE_DATA = 0x01 } esp_partition_type_t;
typedef enum { ESP_PARTITION_SUBTYPE_DATA_NVS = 0x02, ESP_PARTITION_SUBTYPE_ANY = 0xff } esp_partition_subtype_t;
typedef struct { esp_partition_type_t type; esp_partition_subtype_t subtype; char label[17]; } esp_partition_t;
const esp_partition_t* esp_partition_find_first(esp_partition_type_t, esp_partition_subtype_t, const char*);
