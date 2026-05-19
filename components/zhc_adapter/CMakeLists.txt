# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# zhc_adapter — bridges the static-memory embedded ZCL library
# (embedded/zhc/) into the ZHAC P4 firmware for parallel observation.
#
# Only the portable core is linked — host-only scaffolding under
# embedded/zhc/tests and embedded/zhc/tools is skipped.

if(DEFINED ENV{EMBEDDED_ZHC_PATH})
    set(ZHC_ROOT $ENV{EMBEDDED_ZHC_PATH})
else()
    set(ZHC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../embedded/zhc)
endif()

# Pull the full ported device catalog into the firmware. Core +
# vendor registries + every device cpp (hand-authored and generated)
# are compiled into the adapter so `kLumiRegistry[]` / `kTuyaRegistry[]`
# resolve on-device exactly as in host tests.
file(GLOB ZHC_GENERIC_SRCS   ${ZHC_ROOT}/definitions/_generic/*.cpp)
file(GLOB ZHC_LUMI_SRCS      ${ZHC_ROOT}/definitions/lumi/*.cpp)
file(GLOB ZHC_TUYA_SRCS      ${ZHC_ROOT}/definitions/tuya/*.cpp)
file(GLOB ZHC_TUYA_GEN_SRCS  ${ZHC_ROOT}/definitions/tuya/generated/*.cpp)
file(GLOB ZHC_MOES_SRCS      ${ZHC_ROOT}/definitions/moes/*.cpp)
file(GLOB ZHC_MOES_GEN_SRCS  ${ZHC_ROOT}/definitions/moes/generated/*.cpp)
file(GLOB ZHC_ZOSUNG_SRCS    ${ZHC_ROOT}/definitions/zosung/*.cpp)
file(GLOB ZHC_PHI_SRCS       ${ZHC_ROOT}/definitions/philips/*.cpp)
file(GLOB ZHC_PHI_GEN_SRCS   ${ZHC_ROOT}/definitions/philips/generated/*.cpp)
file(GLOB ZHC_SON_SRCS       ${ZHC_ROOT}/definitions/sonoff/*.cpp)
file(GLOB ZHC_SON_GEN_SRCS   ${ZHC_ROOT}/definitions/sonoff/generated/*.cpp)
file(GLOB ZHC_HEI_SRCS       ${ZHC_ROOT}/definitions/heiman/*.cpp)
file(GLOB ZHC_HEI_GEN_SRCS   ${ZHC_ROOT}/definitions/heiman/generated/*.cpp)
file(GLOB ZHC_SAS_SRCS       ${ZHC_ROOT}/definitions/saswell/*.cpp)
file(GLOB ZHC_SAS_GEN_SRCS   ${ZHC_ROOT}/definitions/saswell/generated/*.cpp)
file(GLOB ZHC_EFK_SRCS       ${ZHC_ROOT}/definitions/efekta/*.cpp)
file(GLOB ZHC_EFK_GEN_SRCS   ${ZHC_ROOT}/definitions/efekta/generated/*.cpp)
file(GLOB ZHC_SCH_SRCS       ${ZHC_ROOT}/definitions/schneider/*.cpp)
file(GLOB ZHC_SCH_GEN_SRCS   ${ZHC_ROOT}/definitions/schneider/generated/*.cpp)
file(GLOB ZHC_IKE_SRCS       ${ZHC_ROOT}/definitions/ikea/*.cpp)
file(GLOB ZHC_IKE_GEN_SRCS   ${ZHC_ROOT}/definitions/ikea/generated/*.cpp)
file(GLOB ZHC_INN_SRCS       ${ZHC_ROOT}/definitions/innr/*.cpp)
file(GLOB ZHC_INN_GEN_SRCS   ${ZHC_ROOT}/definitions/innr/generated/*.cpp)
file(GLOB ZHC_GLE_SRCS       ${ZHC_ROOT}/definitions/gledopto/*.cpp)
file(GLOB ZHC_GLE_GEN_SRCS   ${ZHC_ROOT}/definitions/gledopto/generated/*.cpp)
file(GLOB ZHC_SUN_SRCS       ${ZHC_ROOT}/definitions/sunricher/*.cpp)
file(GLOB ZHC_SUN_GEN_SRCS   ${ZHC_ROOT}/definitions/sunricher/generated/*.cpp)
file(GLOB ZHC_NAM_SRCS       ${ZHC_ROOT}/definitions/namron/*.cpp)
file(GLOB ZHC_NAM_GEN_SRCS   ${ZHC_ROOT}/definitions/namron/generated/*.cpp)
file(GLOB ZHC_LED_SRCS       ${ZHC_ROOT}/definitions/ledvance/*.cpp)
file(GLOB ZHC_LED_GEN_SRCS   ${ZHC_ROOT}/definitions/ledvance/generated/*.cpp)
file(GLOB ZHC_OSR_SRCS       ${ZHC_ROOT}/definitions/osram/*.cpp)
file(GLOB ZHC_OSR_GEN_SRCS   ${ZHC_ROOT}/definitions/osram/generated/*.cpp)
file(GLOB ZHC_ADE_SRCS       ${ZHC_ROOT}/definitions/adeo/*.cpp)
file(GLOB ZHC_ADE_GEN_SRCS   ${ZHC_ROOT}/definitions/adeo/generated/*.cpp)

# Tier C glob bundles — collected into a single list for brevity.
set(ZHC_TC_SRCS "")
foreach(TC_V IN ITEMS legrand shinasystem third_reality robb paulmann
                       orvibo slacky_diy nue_3a muller_licht develco
                       iluminize hive feibit)
    file(GLOB _ZHC_TC_BASE ${ZHC_ROOT}/definitions/${TC_V}/*.cpp)
    file(GLOB _ZHC_TC_GEN  ${ZHC_ROOT}/definitions/${TC_V}/generated/*.cpp)
    list(APPEND ZHC_TC_SRCS ${_ZHC_TC_BASE} ${_ZHC_TC_GEN})
endforeach()

set(ZHC_TD_SRCS "")
foreach(TD_V IN ITEMS yale sengled smartthings yokis zemismart ewelink
                       candeo sylvania sinope qa lincukoo bitron
                       aurora_lighting immax bosch shelly owon adurosmart)
    file(GLOB _ZHC_TD_BASE ${ZHC_ROOT}/definitions/${TD_V}/*.cpp)
    file(GLOB _ZHC_TD_GEN  ${ZHC_ROOT}/definitions/${TD_V}/generated/*.cpp)
    list(APPEND ZHC_TD_SRCS ${_ZHC_TD_BASE} ${_ZHC_TD_GEN})
endforeach()

# Tier E — aggregator + every remaining vendor directory.
set(ZHC_TE_SRCS "")
file(GLOB _ZHC_TE_AGG ${ZHC_ROOT}/definitions/_tier_e/*.cpp)
list(APPEND ZHC_TE_SRCS ${_ZHC_TE_AGG})
file(GLOB _ZHC_TE_DIRS LIST_DIRECTORIES true ${ZHC_ROOT}/definitions/*)
foreach(TE_DIR IN LISTS _ZHC_TE_DIRS)
    if(IS_DIRECTORY ${TE_DIR})
        get_filename_component(TE_NAME ${TE_DIR} NAME)
        set(_SKIP _generic _tier_e zosung
            lumi tuya moes philips sonoff heiman saswell efekta schneider
            ikea innr gledopto sunricher namron ledvance osram adeo
            legrand shinasystem third_reality robb paulmann orvibo slacky_diy
            nue_3a muller_licht develco iluminize hive feibit
            yale sengled smartthings yokis zemismart ewelink candeo sylvania
            sinope qa lincukoo bitron aurora_lighting immax bosch shelly
            owon adurosmart)
        list(FIND _SKIP ${TE_NAME} _found)
        if(_found EQUAL -1)
            file(GLOB _ZHC_TE_VBASE ${TE_DIR}/*.cpp)
            file(GLOB _ZHC_TE_VGEN  ${TE_DIR}/generated/*.cpp)
            list(APPEND ZHC_TE_SRCS ${_ZHC_TE_VBASE} ${_ZHC_TE_VGEN})
        endif()
    endif()
endforeach()

idf_component_register(
    SRCS
        "src/zhc_adapter.cpp"
        "src/zhc_adapter_fallback.cpp"
        "${ZHC_ROOT}/src/zcl/header.cpp"
        "${ZHC_ROOT}/src/zcl/foundation.cpp"
        "${ZHC_ROOT}/src/zcl/decoder.cpp"
        "${ZHC_ROOT}/src/zcl/tuya_dp.cpp"
        "${ZHC_ROOT}/src/runtime/dispatch.cpp"
        "${ZHC_ROOT}/src/runtime/definition_runtime.cpp"
        ${ZHC_GENERIC_SRCS}
        ${ZHC_LUMI_SRCS}
        ${ZHC_TUYA_SRCS}
        ${ZHC_TUYA_GEN_SRCS}
        ${ZHC_MOES_SRCS}
        ${ZHC_MOES_GEN_SRCS}
        ${ZHC_ZOSUNG_SRCS}
        ${ZHC_PHI_SRCS}
        ${ZHC_PHI_GEN_SRCS}
        ${ZHC_SON_SRCS}
        ${ZHC_SON_GEN_SRCS}
        ${ZHC_HEI_SRCS}
        ${ZHC_HEI_GEN_SRCS}
        ${ZHC_SAS_SRCS}
        ${ZHC_SAS_GEN_SRCS}
        ${ZHC_EFK_SRCS}
        ${ZHC_EFK_GEN_SRCS}
        ${ZHC_SCH_SRCS}
        ${ZHC_SCH_GEN_SRCS}
        ${ZHC_IKE_SRCS}
        ${ZHC_IKE_GEN_SRCS}
        ${ZHC_INN_SRCS}
        ${ZHC_INN_GEN_SRCS}
        ${ZHC_GLE_SRCS}
        ${ZHC_GLE_GEN_SRCS}
        ${ZHC_SUN_SRCS}
        ${ZHC_SUN_GEN_SRCS}
        ${ZHC_NAM_SRCS}
        ${ZHC_NAM_GEN_SRCS}
        ${ZHC_LED_SRCS}
        ${ZHC_LED_GEN_SRCS}
        ${ZHC_OSR_SRCS}
        ${ZHC_OSR_GEN_SRCS}
        ${ZHC_ADE_SRCS}
        ${ZHC_ADE_GEN_SRCS}
        ${ZHC_TC_SRCS}
        ${ZHC_TD_SRCS}
        ${ZHC_TE_SRCS}
    INCLUDE_DIRS
        "include"
        "${ZHC_ROOT}/include"
    PRIV_INCLUDE_DIRS
        "${ZHC_ROOT}"
    REQUIRES
        esp_timer
        log
        freertos
    PRIV_REQUIRES
        metrics
)

# Inherit the IDF defaults; add -Wall -Wextra for a bit more signal
# without enforcing -Wpedantic — IDF v6 system headers use GCC
# `#include_next` which trips it. Host CMake still runs with
# -Werror -Wpedantic, so real pedantic issues get caught there.
target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wno-missing-field-initializers>
)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
