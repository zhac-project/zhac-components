#pragma once
#include "esp_err.h"
#include <cstddef>
#include <cstdint>
#define NVS_DEFAULT_PART_NAME "nvs"
typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;
typedef enum {
    NVS_TYPE_U8 = 0x01, NVS_TYPE_I8 = 0x11, NVS_TYPE_U16 = 0x02, NVS_TYPE_I16 = 0x12,
    NVS_TYPE_U32 = 0x04, NVS_TYPE_I32 = 0x14, NVS_TYPE_U64 = 0x08, NVS_TYPE_I64 = 0x18,
    NVS_TYPE_STR = 0x21, NVS_TYPE_BLOB = 0x42, NVS_TYPE_ANY = 0xff
} nvs_type_t;
typedef struct { char namespace_name[16]; char key[16]; nvs_type_t type; } nvs_entry_info_t;
typedef void* nvs_iterator_t;
esp_err_t nvs_open(const char*, nvs_open_mode_t, nvs_handle_t*);
esp_err_t nvs_set_blob(nvs_handle_t, const char*, const void*, size_t);
esp_err_t nvs_get_blob(nvs_handle_t, const char*, void*, size_t*);
esp_err_t nvs_set_u16(nvs_handle_t, const char*, uint16_t);
esp_err_t nvs_get_u16(nvs_handle_t, const char*, uint16_t*);
// device_shadow keeps its NVS schema version under a u8 "ver" key — the
// zap_store harness never used u8, so these are added here (defined in
// stubs/nvs_u8_stub.cpp) to keep nvs_stub.cpp a verbatim copy.
esp_err_t nvs_set_u8(nvs_handle_t, const char*, uint8_t);
esp_err_t nvs_get_u8(nvs_handle_t, const char*, uint8_t*);
esp_err_t nvs_erase_key(nvs_handle_t, const char*);
esp_err_t nvs_erase_all(nvs_handle_t);
esp_err_t nvs_commit(nvs_handle_t);
void      nvs_close(nvs_handle_t);
esp_err_t nvs_entry_find(const char*, const char*, nvs_type_t, nvs_iterator_t*);
esp_err_t nvs_entry_next(nvs_iterator_t*);
esp_err_t nvs_entry_info(nvs_iterator_t, nvs_entry_info_t*);
void      nvs_release_iterator(nvs_iterator_t);
// zap_store moves its store between partitions (review 2026-10-24 WC-04) and
// copies entries of every type; defined in stubs/zap_store_platform_stub.cpp.
// This harness has one partition: the "from_partition" open is plain nvs_open.
esp_err_t nvs_open_from_partition(const char*, const char*, nvs_open_mode_t, nvs_handle_t*);
esp_err_t nvs_set_str(nvs_handle_t, const char*, const char*);
esp_err_t nvs_get_str(nvs_handle_t, const char*, char*, size_t*);
esp_err_t nvs_set_i8(nvs_handle_t, const char*, int8_t);
esp_err_t nvs_get_i8(nvs_handle_t, const char*, int8_t*);
esp_err_t nvs_set_i16(nvs_handle_t, const char*, int16_t);
esp_err_t nvs_get_i16(nvs_handle_t, const char*, int16_t*);
esp_err_t nvs_set_u32(nvs_handle_t, const char*, uint32_t);
esp_err_t nvs_get_u32(nvs_handle_t, const char*, uint32_t*);
esp_err_t nvs_set_i32(nvs_handle_t, const char*, int32_t);
esp_err_t nvs_get_i32(nvs_handle_t, const char*, int32_t*);
esp_err_t nvs_set_u64(nvs_handle_t, const char*, uint64_t);
esp_err_t nvs_get_u64(nvs_handle_t, const char*, uint64_t*);
esp_err_t nvs_set_i64(nvs_handle_t, const char*, int64_t);
esp_err_t nvs_get_i64(nvs_handle_t, const char*, int64_t*);
