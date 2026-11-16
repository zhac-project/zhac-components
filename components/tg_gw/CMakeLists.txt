# components/tg_gw/CMakeLists.txt
# A single-chip firmware (zhac-wired-core: P4, S31) sets ZHAC_MQTT_GW_LOCAL_CLIENT
# (see mqtt_gw) and gets tg_gw_local.cpp: the hub posts to api.telegram.org
# itself. The retired dual-chip build keeps its two halves: the P4 forwards over
# HAP, the S3 does the HTTPS.
idf_build_get_property(local_client ZHAC_MQTT_GW_LOCAL_CLIENT)
set(srcs)
if(local_client)
    list(APPEND srcs "tg_gw_local.cpp" "tg_gw_body.cpp")
elseif(IDF_TARGET STREQUAL "esp32p4")
    list(APPEND srcs "tg_gw_p4.cpp")
elseif(IDF_TARGET STREQUAL "esp32s3")
    list(APPEND srcs "tg_gw_s3.cpp")
endif()

idf_component_register(
    SRCS         ${srcs}
    INCLUDE_DIRS "include"
    REQUIRES     hap_protocol hap_json hap_session zap_common esp_http_client nvs_flash mbedtls zap_tls
)
