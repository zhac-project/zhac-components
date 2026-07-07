#pragma once
#include <cstdint>
typedef int      BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;
#define pdTRUE  1
#define pdFALSE 0
#define pdPASS  1
#define pdFAIL  0
#define portMAX_DELAY ((TickType_t)0xFFFFFFFF)
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define configMINIMAL_STACK_SIZE 768
#define configASSERT(x) ((void)0)
