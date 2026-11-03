# components/tg_gw/CMakeLists.txt
# The two halves of the dual-chip Telegram gateway: the P4 forwards over HAP,
# the S3 does the HTTPS. A single-chip firmware (zhac-wired-core: P4/S31) has
# no S3 to forward to; it sets ZHAC_MQTT_GW_LOCAL_CLIENT (see mqtt_gw) and gets
# no sources here, so its own stub answers instead of a HAP send to nowhere
# (review 2026-10-28). The dual-chip build itself is retired.
idf_build_get_property(local_client ZHAC_MQTT_GW_LOCAL_CLIENT)
set(srcs)
if(IDF_TARGET STREQUAL "esp32p4" AND NOT local_client)
    list(APPEND srcs "tg_gw_p4.cpp")
elseif(IDF_TARGET STREQUAL "esp32s3")
    list(APPEND srcs "tg_gw_s3.cpp")
endif()

idf_component_register(
    SRCS         ${srcs}
    INCLUDE_DIRS "include"
    REQUIRES     hap_protocol hap_json hap_session zap_common esp_http_client nvs_flash mbedtls zap_tls
)
