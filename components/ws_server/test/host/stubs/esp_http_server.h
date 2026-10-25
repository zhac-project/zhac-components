// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
// Host stub of the esp_http_server API that ws_server.cpp calls. Left to the test: the
// WebSocket frame writer httpd_ws_send_frame_async (IDF's two-send() writer) and
// httpd_sess_trigger_close (which the test records).
#include <atomic>
#include <cstddef>
#include <cstdint>

typedef int esp_err_t;
#define ESP_OK          0
#define ESP_FAIL        (-1)
#define ESP_ERR_NO_MEM  0x101
#define ESP_ERR_TIMEOUT 0x107

typedef void* httpd_handle_t;
enum httpd_method_t { HTTP_DELETE = 0, HTTP_GET = 1 };   // data frames after the handshake arrive with method 0
struct httpd_req_t { httpd_method_t method; void* user_ctx; };   // user_ctx carries the fake sockfd
enum httpd_ws_type_t { HTTPD_WS_TYPE_CONTINUE = 0, HTTPD_WS_TYPE_TEXT = 1, HTTPD_WS_TYPE_BINARY = 2, HTTPD_WS_TYPE_PONG = 0xA };
enum httpd_ws_client_info_t { HTTPD_WS_CLIENT_INVALID, HTTPD_WS_CLIENT_HTTP, HTTPD_WS_CLIENT_WEBSOCKET };
struct httpd_ws_frame_t { bool final; bool fragmented; httpd_ws_type_t type; uint8_t* payload; size_t len; };
struct httpd_uri_t {
    const char* uri; httpd_method_t method; esp_err_t (*handler)(httpd_req_t*); void* user_ctx;
    bool is_websocket; bool handle_ws_control_frames; const char* supported_subprotocol;
};
typedef bool (*httpd_uri_match_func_t)(const char*, const char*, size_t);
typedef void (*httpd_close_func_t)(httpd_handle_t, int);
typedef esp_err_t (*httpd_open_func_t)(httpd_handle_t, int);
typedef int (*httpd_recv_func_t)(httpd_handle_t, int, char*, size_t, int);
typedef int (*httpd_send_func_t)(httpd_handle_t, int, const char*, size_t, int);
#define HTTPD_SOCK_ERR_FAIL    -1
#define HTTPD_SOCK_ERR_INVALID -2
#define HTTPD_SOCK_ERR_TIMEOUT -3
struct httpd_config_t {
    uint16_t server_port; size_t stack_size; size_t max_open_sockets; size_t max_uri_handlers;
    bool lru_purge_enable; bool keep_alive_enable; int keep_alive_idle, keep_alive_interval, keep_alive_count;
    int recv_wait_timeout, send_wait_timeout; httpd_uri_match_func_t uri_match_fn; httpd_close_func_t close_fn;
    httpd_open_func_t open_fn;
};
#define HTTPD_DEFAULT_CONFIG() httpd_config_t{}

inline const httpd_uri_t* g_ws_uri = nullptr;   // captured by httpd_register_uri_handler
inline httpd_close_func_t g_close_fn = nullptr;  // captured by httpd_start: what httpd runs when a session ends
inline httpd_open_func_t  g_open_fn  = nullptr;  // captured by httpd_start: what httpd runs when a session opens
inline httpd_recv_func_t  g_recv_override = nullptr;   // the session read/write functions open_fn installs
inline httpd_send_func_t  g_send_override = nullptr;
inline httpd_ws_client_info_t g_fd_info_result = HTTPD_WS_CLIENT_WEBSOCKET;   // what httpd_ws_get_fd_info answers
inline std::atomic<int> g_fd_info_calls{0};      // httpd_ws_get_fd_info calls (a broadcaster calls it once per snapshot fd)
inline esp_err_t httpd_start(httpd_handle_t* h, const httpd_config_t* c) {
    *h = reinterpret_cast<httpd_handle_t>(1); g_close_fn = c->close_fn; g_open_fn = c->open_fn; return ESP_OK;
}
inline esp_err_t httpd_sess_set_recv_override(httpd_handle_t, int, httpd_recv_func_t f) { g_recv_override = f; return ESP_OK; }
inline esp_err_t httpd_sess_set_send_override(httpd_handle_t, int, httpd_send_func_t f) { g_send_override = f; return ESP_OK; }
inline esp_err_t httpd_register_uri_handler(httpd_handle_t, const httpd_uri_t* u) { g_ws_uri = u; return ESP_OK; }
inline int httpd_req_to_sockfd(httpd_req_t* r) { return static_cast<int>(reinterpret_cast<intptr_t>(r->user_ctx)); }
inline esp_err_t httpd_req_get_hdr_value_str(httpd_req_t*, const char*, char*, size_t) { return ESP_FAIL; }
inline size_t httpd_req_get_url_query_len(httpd_req_t*) { return 0; }
inline esp_err_t httpd_req_get_url_query_str(httpd_req_t*, char*, size_t) { return ESP_FAIL; }
inline esp_err_t httpd_query_key_value(const char*, const char*, char*, size_t) { return ESP_FAIL; }
// The next frame's length (0: none, and the read fails) and type. Like IDF, a read with len 0
// takes the header and reports the length; a read with the length set takes the payload, which
// sets g_ws_frame_len back to 0.
inline size_t g_ws_frame_len = 0;
inline httpd_ws_type_t g_ws_frame_type = HTTPD_WS_TYPE_TEXT;
inline esp_err_t httpd_ws_recv_frame(httpd_req_t*, httpd_ws_frame_t* f, size_t max_len) {
    if (!g_ws_frame_len) return ESP_FAIL;
    if (f->len == 0) {
        f->type = g_ws_frame_type;
        f->len  = g_ws_frame_len;
        if (max_len == 0) return ESP_OK;
    }
    if (f->len > max_len || !f->payload) return ESP_FAIL;
    for (size_t i = 0; i < f->len; i++) f->payload[i] = 'x';
    g_ws_frame_len = 0;
    return ESP_OK;
}
inline httpd_ws_client_info_t httpd_ws_get_fd_info(httpd_handle_t, int) { g_fd_info_calls++; return g_fd_info_result; }
inline bool httpd_uri_match_wildcard(const char*, const char*, size_t) { return true; }
esp_err_t httpd_ws_send_frame_async(httpd_handle_t, int fd, httpd_ws_frame_t* frame);   // supplied by the test
esp_err_t httpd_sess_trigger_close(httpd_handle_t, int fd);                             // supplied by the test
