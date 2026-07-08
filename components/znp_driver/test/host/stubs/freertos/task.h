// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host shim for the FreeRTOS task API. znp_confirm.cpp references vTaskDelay in
// its (never-taken on host) init-race branch, so the symbol must resolve at
// link time even though it is a no-op. The RX/worker tasks are NOT compiled by
// this harness, so xTaskCreate* is intentionally absent.
#pragma once
#include "FreeRTOS.h"

typedef void* TaskHandle_t;

// Defined in host_stubs.cpp (external linkage → resolvable at link time).
void vTaskDelay(TickType_t ticks);
