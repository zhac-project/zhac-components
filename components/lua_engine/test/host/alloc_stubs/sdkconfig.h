// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Host stand-in for the IDF sdkconfig.h: what lua_alloc.c reads. The threshold comes from the build (-D).
#pragma once
#define CONFIG_LUA_ENGINE_HEAP_KB 64
