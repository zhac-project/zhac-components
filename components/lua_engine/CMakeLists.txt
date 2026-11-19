# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# Every target: zhac-wired-core runs Lua on the P4 and the S31. `lua` comes from idf_component.yml.
idf_component_register(
    SRCS "src/lua_alloc.c"
         "src/lua_engine.c"
         "src/lua_scheduler.cpp"
         "src/lua_sandbox.c"
         "src/lua_require.c"
         "src/zhac_lua_module.cpp"
         "src/lua_engine_event_bridge.cpp"
         "src/lua_engine_rules_hook.cpp"
         "src/lua_script_cache.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES freertos esp_timer lua
    PRIV_REQUIRES spiffs esp_partition event_bus metrics cron_parser
                  zap_common zap_store zhc_adapter
                  device_cmd
                  device_shadow mqtt_gw tg_gw zigbee_mgr
                  simple_rules lua_cjson
)
include(${CMAKE_CURRENT_LIST_DIR}/lua_limits.cmake)   # the Lua C-stack limits
