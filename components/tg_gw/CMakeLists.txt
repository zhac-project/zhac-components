# components/tg_gw/CMakeLists.txt
set(srcs)
if(IDF_TARGET STREQUAL "esp32p4")
    list(APPEND srcs "tg_gw_p4.cpp")
elseif(IDF_TARGET STREQUAL "esp32s3")
    list(APPEND srcs "tg_gw_s3.cpp")
endif()

idf_component_register(
    SRCS         ${srcs}
    INCLUDE_DIRS "include"
    REQUIRES     hap_protocol hap_json hap_session esp_http_client nvs_flash mbedtls
)
