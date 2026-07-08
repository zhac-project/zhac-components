// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host shim for the FreeRTOS queue API. znp_internal.h declares an
// `extern QueueHandle_t znp_uart_evt_q` (owned by znp_transport.cpp, which this
// harness does NOT compile), so only the QueueHandle_t type is needed here. The
// queue operations live in the RX/worker TUs that are out of scope.
#pragma once
#include "FreeRTOS.h"

typedef void* QueueHandle_t;
