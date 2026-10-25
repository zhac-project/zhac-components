// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Review 2026-10-24 HS-03 and HS-04, on the real ws_server.cpp over the stubs:
//   - a request that runs past the deadline from its first byte fails its reads,
//     an answer restarts the clock, a WebSocket session is exempt;
//   - while a token is set, a socket that has not signed in within 5 s is closed,
//     and a full table closes the oldest unsigned socket to admit a new one.
// A fresh process, so the client table starts empty.
#include "ws_server.h"
#include "esp_timer.h"

#include <cstdarg>
#include <cstdio>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

static int s_fail = 0;
#define CHECK(c, m) do { if (!(c)) { s_fail++; std::printf("FAIL: %s\n", m); } else std::printf("PASS: %s\n", m); } while (0)

static std::vector<int> g_closes;   // what ws_server asked httpd to close
esp_err_t httpd_sess_trigger_close(httpd_handle_t, int fd) { g_closes.push_back(fd); return ESP_OK; }
esp_err_t httpd_ws_send_frame_async(httpd_handle_t, int, httpd_ws_frame_t*) { return ESP_OK; }
void host_log(const char*, const char*, ...) {}

static void add_client(int fd) {            // the handshake registers fd as a client
    httpd_req_t req{HTTP_GET, reinterpret_cast<void*>(static_cast<intptr_t>(fd))};
    g_ws_uri->handler(&req);
}
static bool closed(int fd) {
    for (int c : g_closes) if (c == fd) return true;
    return false;
}

int main() {
    ws_server_init();

    // ── HS-04: the sign-in deadline and the full table ────────────────────
    ws_server_set_api_token("0123456789abcdef0123456789abcdef");
    g_now_us = 100'000'000;
    add_client(101);                         // never signs in
    add_client(102);
    ws_server_fd_set_authed(102);            // signs in with its first frame
    g_now_us += 4'000'000;
    ws_server_sign_in_sweep(g_now_us);
    CHECK(g_closes.empty(), "sign-in deadline: nothing is closed within 5 s");
    g_now_us += 2'000'000;
    ws_server_sign_in_sweep(g_now_us);
    CHECK(closed(101) && !closed(102), "sign-in deadline: the unsigned socket goes after 5 s, the signed-in one stays");
    g_close_fn(nullptr, 101);                // httpd runs the close

    g_closes.clear();
    g_now_us += 1000; add_client(103);       // the table: 102 (signed in), 103, 104 (not yet)
    g_now_us += 1000; add_client(104);
    g_now_us += 1000; add_client(105);       // full: the oldest unsigned (103) makes room
    CHECK(closed(103) && !closed(104) && !closed(102) && !closed(105) && ws_server_client_count() == 3,
          "full table: the oldest socket that has not signed in is closed to admit the new one");
    g_closes.clear();
    httpd_req_t frame{HTTP_DELETE, reinterpret_cast<void*>(static_cast<intptr_t>(103))};
    g_ws_uri->handler(&frame);               // a frame from 103 before its close ran
    CHECK(g_closes.empty() && ws_server_client_count() == 3, "a socket being closed is not adopted again by its next frame");
    g_close_fn(nullptr, 103);

    ws_server_set_api_token(nullptr);
    g_now_us += 10'000'000;
    ws_server_sign_in_sweep(g_now_us);
    CHECK(!closed(104), "sign-in deadline: nothing is closed while no token is set");
    for (int fd : {102, 104, 105}) g_close_fn(nullptr, fd);

    // ── HS-03: the request deadline ───────────────────────────────────────
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    CHECK(g_open_fn && g_open_fn(nullptr, sv[0]) == ESP_OK && g_recv_override && g_send_override,
          "open_fn installs the read and write functions");
    g_fd_info_result = HTTPD_WS_CLIENT_HTTP;
    char buf[8];
    (void)!write(sv[1], "GET ", 4);
    CHECK(g_recv_override(nullptr, sv[0], buf, 2, 0) == 2, "a request's first read starts its clock");
    g_now_us += 7'000'000;
    CHECK(g_recv_override(nullptr, sv[0], buf, 2, 0) == 2, "reads within the deadline go through");
    g_now_us += 2'000'000;
    CHECK(g_recv_override(nullptr, sv[0], buf, 2, 0) == HTTPD_SOCK_ERR_FAIL, "a read past 8 s from the first byte fails");
    CHECK(g_send_override(nullptr, sv[0], "HTTP/1.1", 8, 0) == 8, "the answer goes out");
    (void)!write(sv[1], "GET ", 4);
    g_now_us += 60'000'000;
    CHECK(g_recv_override(nullptr, sv[0], buf, 4, 0) == 4, "after an answer the next request starts a fresh clock");
    g_fd_info_result = HTTPD_WS_CLIENT_WEBSOCKET;
    (void)!write(sv[1], "x", 1);
    g_now_us += 60'000'000;
    CHECK(g_recv_override(nullptr, sv[0], buf, 1, 0) == 1, "a WebSocket session has no request deadline");
    g_close_fn(nullptr, sv[0]);
    close(sv[1]);

    std::printf("%s (%d failure(s))\n", s_fail ? "FAILED" : "ALL PASS", s_fail);
    return s_fail ? 1 : 0;
}
