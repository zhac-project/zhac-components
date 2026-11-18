// Host stand-in for the IDF sdkconfig.h: what lua_alloc.c reads. The threshold comes from the build (-D).
#pragma once
#define CONFIG_LUA_ENGINE_HEAP_KB 64
