#pragma once
#include <stdio.h>
#define ESP_LOGW(tag, ...) (printf("W %s: ", tag), printf(__VA_ARGS__), printf("\n"))
#define ESP_LOGI(tag, ...) (printf("I %s: ", tag), printf(__VA_ARGS__), printf("\n"))
#define ESP_LOGE(tag, ...) (printf("E %s: ", tag), printf(__VA_ARGS__), printf("\n"))
