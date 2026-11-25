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
#include "esp_heap_caps.h"     // g_psram_full, g_internal_allocs, g_internal_caps
#include "esp_timer.h"
#include "freertos/semphr.h"   // t_nheld: the locks this thread holds

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

static int s_fail = 0;
#define CHECK(c, m) do { if (!(c)) { s_fail++; std::printf("FAIL: %s\n", m); } else std::printf("PASS: %s\n", m); } while (0)

static std::vector<int> g_closes;   // what ws_server asked httpd to close
esp_err_t httpd_sess_trigger_close(httpd_handle_t, int fd) { g_closes.push_back(fd); return ESP_OK; }
// Records what was written, and whether a lock (s_tx_mutex: nothing else is
// held while a frame goes out) was held while writing it (fix review N1).
esp_err_t httpd_ws_send_frame_async(httpd_handle_t, int, httpd_ws_frame_t* f) {
    g_sent_frame = StubSentFrame{f->type, f->len, f->len && f->payload ? f->payload[0] : uint8_t{0}};
    g_sent_frames++;
    g_sent_under_lock = t_nheld > 0;
    return ESP_OK;
}
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
    // Between frames httpd reads only the next opcode byte; every frame, control
    // frames included, is timed in ws_handler (R-04, below).
    CHECK(g_recv_override(nullptr, sv[0], buf, 1, 0) == 1,
          "a WebSocket session has no request clock between frames");
    g_close_fn(nullptr, sv[0]);
    close(sv[1]);

    // ── HS-12: a frame over the cap ends its session ──────────────────────
    // Skipping it left its payload in the stream, read as the next frame's header.
    static size_t s_rx_len = 0;
    ws_server_set_rx_callback([](int, const char*, size_t len) { s_rx_len = len; });
    add_client(110);
    httpd_req_t f110{HTTP_DELETE, reinterpret_cast<void*>(static_cast<intptr_t>(110))};
    g_ws_frame_len = 32 * 1024;
    CHECK(g_ws_uri->handler(&f110) == ESP_OK && s_rx_len == 32 * 1024, "a frame of 32 KB is read and handed on");
    const int before = ws_server_client_count();
    s_rx_len = 0;
    g_ws_frame_len = 32 * 1024 + 1;
    CHECK(g_ws_uri->handler(&f110) == ESP_FAIL && s_rx_len == 0 && ws_server_client_count() == before - 1,
          "a frame over 32 KB fails its session (httpd closes it) and leaves the client table");
    CHECK(g_ws_frame_len == 32 * 1024 + 1,
          "... without reading its payload away (that would hold the httpd task as long as the client trickles it)");
    g_ws_frame_len = 0;   // the stub's pending payload: httpd drops it with the session
    g_close_fn(nullptr, 110);
    // A frame's buffer comes from PSRAM. With PSRAM short, internal RAM stands in only for a frame of at most
    // 8 KB, the cap before the 32 KB one: four 32 KB frames would take what the radio and lwIP need (review F11, C1).
    add_client(112);
    httpd_req_t f112{HTTP_DELETE, reinterpret_cast<void*>(static_cast<intptr_t>(112))};
    g_psram_full = true;
    g_internal_allocs = 0;
    s_rx_len = 0;
    g_ws_frame_len = 32 * 1024;
    CHECK(g_ws_uri->handler(&f112) == ESP_ERR_NO_MEM && s_rx_len == 0 && g_internal_allocs == 0,
          "PSRAM full: a 32 KB frame is refused, and takes no internal RAM");
    g_ws_frame_len = 8 * 1024;   // the refused frame's payload was never read; httpd ends that session
    CHECK(g_ws_uri->handler(&f112) == ESP_OK && s_rx_len == 8 * 1024 && g_internal_allocs == 1,
          "PSRAM full: a frame of 8 KB still falls back to internal RAM");
    CHECK(g_internal_caps == (MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL),
          "... to the RAM malloc takes, not to the driver reserve (MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL)");
    g_ws_frame_len = 8 * 1024 + 1;
    s_rx_len = 0;
    CHECK(g_ws_uri->handler(&f112) == ESP_ERR_NO_MEM && s_rx_len == 0 && g_internal_allocs == 1,
          "PSRAM full: one byte more is refused");
    g_ws_frame_len = 0;
    g_psram_full = false;
    g_close_fn(nullptr, 112);
    // Every frame that reaches the handler is read to its end: binary frames,
    // continuations and (IDF 6.1) PONGs too. Only text is handed on.
    add_client(111);
    httpd_req_t f111{HTTP_DELETE, reinterpret_cast<void*>(static_cast<intptr_t>(111))};
    for (httpd_ws_type_t t : {HTTPD_WS_TYPE_BINARY, HTTPD_WS_TYPE_CONTINUE, HTTPD_WS_TYPE_PONG}) {
        g_ws_frame_type = t;
        g_ws_frame_len = 100;
        s_rx_len = 0;
        CHECK(g_ws_uri->handler(&f111) == ESP_OK && g_ws_frame_len == 0 && s_rx_len == 0,
              "a binary, continuation or PONG frame is read away, not handed on");
    }
    g_ws_frame_type = HTTPD_WS_TYPE_BINARY;
    g_ws_frame_len = 32 * 1024 + 1;
    CHECK(g_ws_uri->handler(&f111) == ESP_FAIL, "a binary frame over 32 KB fails its session too");
    g_ws_frame_len = 0;
    g_ws_frame_type = HTTPD_WS_TYPE_TEXT;
    g_close_fn(nullptr, 111);

    // ── R-04 (review 2026-10-28): a trickled frame ends its session ────────
    // Each byte arrives just inside the 3 s receive timeout; only a deadline on
    // the whole frame stops one frame holding the httpd task (a 32 KB frame:
    // about 27 h). Two sockets: one trickles past 8 s, one sends two frames of
    // 5 s each (the deadline is per frame, not per session).
    {
        char bytes[200];
        std::memset(bytes, 'y', sizeof(bytes));
        g_fd_info_result = HTTPD_WS_CLIENT_WEBSOCKET;

        int slow[2];
        socketpair(AF_UNIX, SOCK_STREAM, 0, slow);
        (void)g_open_fn(nullptr, slow[0]);
        add_client(slow[0]);
        httpd_req_t fs{HTTP_DELETE, reinterpret_cast<void*>(static_cast<intptr_t>(slow[0]))};
        (void)!write(slow[1], bytes, 100);
        g_ws_trickle_us = 2'900'000;
        g_ws_frame_len = 100;
        s_rx_len = 0;
        CHECK(g_ws_uri->handler(&fs) == ESP_FAIL && s_rx_len == 0,
              "a frame trickled past 8 s fails its session");
        g_ws_frame_len = 0;
        g_close_fn(nullptr, slow[0]);
        close(slow[1]);

        int ok[2];
        socketpair(AF_UNIX, SOCK_STREAM, 0, ok);
        (void)g_open_fn(nullptr, ok[0]);
        add_client(ok[0]);
        httpd_req_t fo{HTTP_DELETE, reinterpret_cast<void*>(static_cast<intptr_t>(ok[0]))};
        (void)!write(ok[1], bytes, 200);
        g_ws_trickle_us = 50'000;                // 100 bytes: 5 s
        g_ws_frame_len = 100;
        s_rx_len = 0;
        const bool first = g_ws_uri->handler(&fo) == ESP_OK && s_rx_len == 100;
        g_ws_frame_len = 100;
        s_rx_len = 0;
        CHECK(first && g_ws_uri->handler(&fo) == ESP_OK && s_rx_len == 100,
              "frames that each arrive within 8 s pass, however long the session");
        g_ws_trickle_us = 0;
        g_close_fn(nullptr, ok[0]);
        close(ok[1]);
    }

    // ── R-04, control frames (review B1) ─────────────────────────────────
    // While the URI did not ask for control frames, httpd read a PING or CLOSE
    // itself, outside ws_handler and so outside the frame deadline: one trickled
    // PING held the httpd task about 6 minutes. They come to the handler now,
    // which answers them as httpd did.
    {
        CHECK(g_ws_uri->handle_ws_control_frames,
              "control frames come to the handler, so their reads have the frame deadline");
        add_client(120);
        httpd_req_t fc{HTTP_DELETE, reinterpret_cast<void*>(static_cast<intptr_t>(120))};
        g_sent_frames = 0;
        g_ws_frame_type = HTTPD_WS_TYPE_PING;
        g_ws_frame_len = 5;
        s_rx_len = 0;
        CHECK(g_ws_uri->handler(&fc) == ESP_OK && g_sent_frames == 1 && g_sent_frame.type == HTTPD_WS_TYPE_PONG &&
              g_sent_frame.len == 5 && g_sent_frame.first == 'x' && s_rx_len == 0,
              "a PING is answered with a PONG carrying its payload, and is not handed on");
        CHECK(g_sent_under_lock, "the PONG is written under the send lock, so a broadcast cannot interleave with it");
        g_sent_frames = 0;
        g_ws_frame_type = HTTPD_WS_TYPE_PONG;
        g_ws_frame_len = 3;
        CHECK(g_ws_uri->handler(&fc) == ESP_OK && g_sent_frames == 0 && g_ws_frame_len == 0,
              "a PONG is read away and not answered");
        g_ws_frame_type = HTTPD_WS_TYPE_PING;
        g_ws_frame_len = 126;
        CHECK(g_ws_uri->handler(&fc) == ESP_FAIL && g_sent_frames == 0,
              "a control frame over 125 bytes fails its session (RFC 6455 5.5)");
        g_ws_frame_len = 0;
        g_ws_frame_type = HTTPD_WS_TYPE_CLOSE;
        g_ws_frame_len = 2;
        CHECK(g_ws_uri->handler(&fc) == ESP_OK && g_sent_frames == 1 && g_sent_frame.type == HTTPD_WS_TYPE_CLOSE &&
              g_sent_under_lock, "a CLOSE is answered with a CLOSE under the send lock (httpd then ends the session)");
        g_close_fn(nullptr, 120);

        int tp[2];
        socketpair(AF_UNIX, SOCK_STREAM, 0, tp);
        (void)g_open_fn(nullptr, tp[0]);
        add_client(tp[0]);
        httpd_req_t ft{HTTP_DELETE, reinterpret_cast<void*>(static_cast<intptr_t>(tp[0]))};
        char bytes[125];
        std::memset(bytes, 'p', sizeof(bytes));
        (void)!write(tp[1], bytes, sizeof(bytes));
        g_sent_frames = 0;
        g_ws_trickle_us = 2'900'000;
        g_ws_frame_type = HTTPD_WS_TYPE_PING;
        g_ws_frame_len = 125;
        CHECK(g_ws_uri->handler(&ft) == ESP_FAIL && g_sent_frames == 0,
              "a PING trickled past 8 s fails its session");
        g_ws_trickle_us = 0;
        g_ws_frame_len = 0;
        g_ws_frame_type = HTTPD_WS_TYPE_TEXT;
        g_close_fn(nullptr, tp[0]);
        close(tp[1]);
    }

    std::printf("%s (%d failure(s))\n", s_fail ? "FAILED" : "ALL PASS", s_fail);
    return s_fail ? 1 : 0;
}
