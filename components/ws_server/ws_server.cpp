// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ws_server.h"
#include "utf8_seq.h"   // utf8_valid / utf8_scrub: the send guard in send_locked
#include "sdkconfig.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <atomic>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

// httpd_sess_trigger_close (adopt_client, send_locked) queues work from the
// httpd task itself. With CONFIG_HTTPD_QUEUE_WORK_BLOCKING a full control queue
// would block that task for good: only it drains the queue.
#if CONFIG_HTTPD_QUEUE_WORK_BLOCKING
#error "ws_server needs CONFIG_HTTPD_QUEUE_WORK_BLOCKING=n: it queues httpd work from the httpd task, so a full queue would deadlock it"
#endif

// DS10 (DS_FINDINGS): cap on an inbound WS frame; a frame beyond this closes
// its session (a misbehaving/hostile client can't force a huge per-frame
// allocation). 32 KB: the hub page saves the whole Safety config in one frame
// (alarm.config.set), about 22 KB for the largest config its limits allow;
// rule DSL and a short script.check source are far below. The SPA sends
// nothing bigger: on wired and single-chip hubs it checks a long script over
// REST, and on the dual-chip hub it does not check it as it is typed.
#define WS_RX_MAX (32 * 1024)
// The largest frame read into internal RAM when PSRAM is short (the cap before the 32 KB one).
#define WS_RX_INTERNAL_MAX (8 * 1024)

static const char*       TAG      = "ws_server";
static httpd_handle_t    s_server = nullptr;
static SemaphoreHandle_t s_mutex  = nullptr;
static SemaphoreHandle_t s_tx_mutex = nullptr;  // one frame write at a time (broadcast or reply); taken before s_mutex, never after
static WsRxCallback      s_rx_cb  = nullptr;
static char              s_api_token[33] = {};

#define MAX_WS_CLIENTS 3
static int     s_fds[MAX_WS_CLIENTS];
static bool    s_fd_authed[MAX_WS_CLIENTS] = {};   // F18: per-fd WS auth state
static int64_t s_fd_since[MAX_WS_CLIENTS]  = {};   // when it joined (HS-04 sign-in deadline)
static int     s_fd_count = 0;

// Review 2026-10-24 HS-04: while a token is set, a socket that has not signed
// in within this long is closed, and a full table makes room by closing the
// oldest such socket rather than refusing the new one -- three idle unsigned
// sockets (any page on the LAN can open them) used to lock the hub page out.
static constexpr int64_t kSignInDeadlineUs = 5 * 1000 * 1000;

// Sockets this server asked httpd to close: a frame they send before the close
// runs must not adopt them again (ensure_fd). Cleared by close_fn. Under s_mutex.
static int s_closing[MAX_WS_CLIENTS * 2];
static int s_closing_n = 0;

static bool closing_has_locked(int fd) {
    for (int i = 0; i < s_closing_n; i++) if (s_closing[i] == fd) return true;
    return false;
}

// Close a client socket from any task; nothing logged (callers may sit under
// the log sink).
static void close_client(int fd) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!closing_has_locked(fd)) {
        if (s_closing_n == (int)(sizeof(s_closing) / sizeof(s_closing[0]))) s_closing_n--;   // drop the oldest
        for (int i = s_closing_n; i > 0; i--) s_closing[i] = s_closing[i - 1];
        s_closing[0] = fd;
        s_closing_n++;
    }
    xSemaphoreGive(s_mutex);
    httpd_sess_trigger_close(s_server, fd);
}

// Single-entry hook table. Suitable while there's only one
// non-httpd transport (remote_client). Easy to extend to a small
// array if a second transport needs the same plumbing.
static int          s_hook_fd   = 0;       // 0 = unused (legal httpd fd value)
static WsReplyHook  s_hook_func = nullptr;

// Returns false when the client table is full (caller closes the socket —
// a slot-less socket would stay open, accept commands, yet never receive a
// single broadcast: a half-functional zombie).
//
// No ESP_LOG* while s_mutex is held — here, in remove_fd, or anywhere else in
// this file. The wired and single-chip WS log sinks call ws_server_broadcast
// from inside the log call, and that takes s_mutex again: a self-deadlock on
// this non-recursive mutex. Collect what to log, unlock, then log.
static bool add_fd(int fd, bool authed) {
    bool added = false;
    int  total = 0;
    int  evicted = -1;
    const int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_fd_count; i++) {
        if (s_fds[i] == fd) { s_fd_authed[i] = authed; xSemaphoreGive(s_mutex); return true; }
    }
    int slot = -1;
    if (s_fd_count < MAX_WS_CLIENTS) {
        slot = s_fd_count++;
    } else if (s_api_token[0]) {
        // Full: the oldest socket that has not signed in makes room (HS-04).
        for (int i = 0; i < s_fd_count; i++)
            if (!s_fd_authed[i] && (slot < 0 || s_fd_since[i] < s_fd_since[slot])) slot = i;
        if (slot >= 0) evicted = s_fds[slot];
    }
    if (slot >= 0) {
        s_fds[slot]       = fd;
        s_fd_authed[slot] = authed;
        s_fd_since[slot]  = now;
        added = true;
        total = s_fd_count;
    }
    xSemaphoreGive(s_mutex);
    if (evicted >= 0) {
        close_client(evicted);
        ESP_LOGW(TAG, "WS client table full: closed fd=%d, which had not signed in", evicted);
    }
    if (added) ESP_LOGI(TAG, "WS client added fd=%d authed=%d total=%d", fd, authed, total);
    return added;
}

// httpd gives every accepted socket SO_SNDTIMEO = send_wait_timeout (10 s, see
// ws_server_init), per send() call, and a WS frame is two of them. A dead peer
// (no FIN, no RST) therefore stalls a send for that long WHILE it holds
// s_tx_mutex, and every other sender waits with it — the single httpd task too,
// whenever a reply is queued behind it. A LAN client drains the 11.5 KB send
// buffer in milliseconds, so WS sockets get a much shorter cap (the plain HTTP
// part of a connection keeps the 10 s).
static constexpr time_t WS_SEND_TIMEOUT_S = 3;

// Make a socket a broadcast client: cap its send stall, then add it. Closing on a
// full table is deliberate — a slot-less socket would stay open, accept commands,
// yet never receive a single broadcast (a half-functional zombie). Used for a
// fresh handshake (register_client) and for a socket first seen on a data frame
// (ensure_fd).
static void adopt_client(int fd, bool authed) {
    const timeval tv = {WS_SEND_TIMEOUT_S, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if (!add_fd(fd, authed)) {
        ESP_LOGW(TAG, "WS client limit (%d) reached, closing fd=%d",
                 MAX_WS_CLIENTS, fd);
        close_client(fd);
    }
}

// Add the fd ONLY if absent, leaving an existing entry's auth flag alone.
//
// add_fd() overwrites s_fd_authed on a repeat call, which is right at handshake
// (freshly detected auth) and wrong everywhere else — calling it per received
// frame would deauthenticate a client on its very next message. Hence the split.
static void ensure_fd(int fd, bool authed_if_new) {
    bool present = false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_fd_count; i++) {
        if (s_fds[i] == fd) { present = true; break; }
    }
    const bool closing = closing_has_locked(fd);   // its close is queued: not a client any more
    xSemaphoreGive(s_mutex);
    if (!present && !closing) adopt_client(fd, authed_if_new);
}

// Take the fd out of the table WITHOUT logging; returns the table size afterwards,
// or -1 if the fd was not in it. For send_locked, which holds s_tx_mutex and must
// not log under it; everything else calls remove_fd().
static int remove_fd_quiet(int fd) {
    int left = -1;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_fd_count; i++) {
        if (s_fds[i] == fd) {
            --s_fd_count;
            s_fds[i]       = s_fds[s_fd_count];
            s_fd_authed[i] = s_fd_authed[s_fd_count];   // F18: keep auth state aligned
            s_fd_since[i]  = s_fd_since[s_fd_count];
            left = s_fd_count;
            break;
        }
    }
    xSemaphoreGive(s_mutex);
    return left;
}

// True if the fd was in the table and this call removed it. Logs after
// unlocking, like add_fd.
static bool remove_fd(int fd) {
    const int left = remove_fd_quiet(fd);
    if (left >= 0) ESP_LOGI(TAG, "WS client removed fd=%d total=%d", fd, left);
    return left >= 0;
}

// F18: per-fd auth state for first-message WS authentication.
bool ws_server_fd_is_authed(int fd) {
    bool authed = false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_fd_count; i++) {
        if (s_fds[i] == fd) { authed = s_fd_authed[i]; break; }
    }
    xSemaphoreGive(s_mutex);
    return authed;
}
void ws_server_fd_set_authed(int fd) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_fd_count; i++) {
        if (s_fds[i] == fd) { s_fd_authed[i] = true; break; }
    }
    xSemaphoreGive(s_mutex);
}
// Q48 (QWEN_FINDINGS triage): drop all per-fd auth so a token rotation actually
// invalidates live sessions — old-token sockets must re-auth with the new token.
void ws_server_fd_deauth_all() {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_fd_count; i++) s_fd_authed[i] = false;
    xSemaphoreGive(s_mutex);
}

// Review 2026-10-24 HS-01: once sign-in is off the dispatcher trusts every
// socket, so one that never signed in -- a page of another site may have
// opened it while sign-in was on -- must not stay. Its reconnect then meets the
// upgrade filter. Closed outside the lock (close_fn takes it).
void ws_server_close_unauthed() {
    int fds[MAX_WS_CLIENTS];
    int n = 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_fd_count; i++) if (!s_fd_authed[i]) fds[n++] = s_fds[i];
    xSemaphoreGive(s_mutex);
    for (int i = 0; i < n; i++) close_client(fds[i]);
}

// HS-04: while a token is set, close the sockets that joined more than
// kSignInDeadlineUs before `now_us` and have not signed in. The browser page
// signs in with its first frame, so only a socket nobody signs in on goes.
// Runs once a second from ws_server_init's timer.
void ws_server_sign_in_sweep(int64_t now_us) {
    int fds[MAX_WS_CLIENTS];
    int n = 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_api_token[0])
        for (int i = 0; i < s_fd_count; i++)
            if (!s_fd_authed[i] && now_us - s_fd_since[i] > kSignInDeadlineUs) fds[n++] = s_fds[i];
    xSemaphoreGive(s_mutex);
    for (int i = 0; i < n; i++) close_client(fds[i]);
}

// F18 (FINDINGS.md): the socket may OPEN unauthenticated; auth is enforced
// per-message in the dispatch layer. If the client presents a valid token at
// handshake (X-Api-Key header, or the legacy ?token= query) we mark it authed
// now — back-compat for non-browser clients, which set headers without any URL
// leak. Browsers connect WITHOUT a URL token and authenticate via a first
// {"cmd":"auth"} message, keeping the long-lived token out of proxy/access logs.
static bool detect_handshake_auth(httpd_req_t* req) {
    if (s_api_token[0] == '\0') return true;   // no token configured → auth off

    char key[64] = {};
    bool got_key = false;
    if (httpd_req_get_hdr_value_str(req, "X-Api-Key", key, sizeof(key)) == ESP_OK) {
        got_key = true;
    } else {
        size_t qlen = httpd_req_get_url_query_len(req);
        if (qlen > 0 && qlen < 256) {
            char qbuf[256];
            if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK &&
                httpd_query_key_value(qbuf, "token", key, sizeof(key)) == ESP_OK) {
                got_key = true;
            }
        }
    }
    if (got_key && strlen(key) == 32) {
        // Constant-time compare: XOR-accumulate all 32 bytes.
        uint8_t diff = 0;
        for (int i = 0; i < 32; i++)
            diff |= (uint8_t)key[i] ^ (uint8_t)s_api_token[i];
        return diff == 0;
    }
    return false;
}

// The firmware's cross-site filter for the upgrade (review 2026-10-24 HS-01):
// a browser opens a WebSocket for any page it shows, with no CORS check, so
// the page's Origin is all that tells the hub's own page from another site's.
static WsUpgradeFilter s_upgrade_filter = nullptr;

static bool upgrade_allowed(httpd_req_t* req) {
    return !s_upgrade_filter || s_upgrade_filter(req);
}

#ifdef CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT
// Before the 101: a refused upgrade never becomes a socket.
static esp_err_t ws_pre_handshake(httpd_req_t* req) {
    return upgrade_allowed(req) ? ESP_OK : ESP_FAIL;
}
#endif

// Register a freshly handshaken socket (see adopt_client). Without the
// pre-handshake callback the filter runs here, after the 101, and a refused
// socket is closed before it is ever registered.
static void register_client(httpd_req_t* req, int fd) {
    if (!upgrade_allowed(req)) {
        close_client(fd);
        return;
    }
    adopt_client(fd, detect_handshake_auth(req));
}

#ifdef CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT
// ESP-IDF v6.1 STOPPED invoking the URI handler for the WebSocket handshake:
//
//   /* If the request is websocket handshake, then do not call the uri->handler */
//   return ESP_OK;                       -- components/esp_http_server/src/httpd_uri.c
//
// v6.0 fell through to "Invoke handler", which is how the HTTP_GET branch below
// ever ran. On v6.1 it never runs, so no client was registered, client_count()
// stayed 0 and ws_server_broadcast() iterated an empty list: every push event
// silently vanished while request/reply kept working — the failure looks like a
// connected, healthy socket that just never receives anything.
//
// v6.1 added this callback for exactly this purpose. It still gets the original
// httpd_req_t, so handshake-time token auth keeps working unchanged.
static esp_err_t ws_post_handshake(httpd_req_t* req) {
    register_client(req, httpd_req_to_sockfd(req));
    return ESP_OK;
}
#endif

static void ws_frame_clock(int fd, bool start);   // the frame deadline (R-04), with the request clocks below
static esp_err_t send_control(int fd, httpd_ws_frame_t* pkt);   // under s_tx_mutex, with the senders below

static esp_err_t ws_handler(httpd_req_t* req) {
    int fd = httpd_req_to_sockfd(req);
    if (req->method == HTTP_GET) {
        // IDF <= 6.0 path. On 6.1+ this is unreachable (see ws_post_handshake).
        register_client(req, fd);
        return ESP_OK;
    }
    // The rest of the frame (header and payload) must arrive within the
    // request deadline: httpd_ws_recv_frame reads through deadline_recv.
    struct FrameClock {
        int fd;
        explicit FrameClock(int f) : fd(f) { ws_frame_clock(fd, true); }
        ~FrameClock() { ws_frame_clock(fd, false); }
    } frame_clock(fd);

    // Safety net, and the ONLY registration that works on IDF 6.1 when
    // CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT is off: adopt any socket that
    // reaches us with a data frame but was never registered. ensure_fd (not
    // add_fd) so a client that authenticated earlier is not knocked back to
    // unauthenticated by its next message.
    //
    // A socket adopted here has no handshake headers to inspect, so it starts
    // unauthenticated whenever a token is configured — it receives no
    // broadcasts until its {"cmd":"auth"} message calls
    // ws_server_fd_set_authed(). That is the safe direction to be wrong in, and
    // it also repairs auth itself: previously that setter silently did nothing
    // because the fd was not in the table to begin with. With an upgrade filter
    // installed it starts unauthenticated even without a token: it skipped the
    // handshake, and with it the filter (a refused socket whose close is still
    // queued must not come in this way).
    ensure_fd(fd, s_api_token[0] == '\0' && !s_upgrade_filter);
    // DS10 (DS_FINDINGS): the old fixed 256-byte stack buffer truncated/rejected
    // any WS frame larger than 255 bytes (rule DSL, script.check Lua source) and
    // a 2 KB+ stack buffer would risk the httpd task's canary on its deep call
    // chains. Probe the length (max_len=0, no copy), then read the whole frame
    // into a right-sized PSRAM buffer.
    httpd_ws_frame_t pkt{};
    esp_err_t ret = httpd_ws_recv_frame(req, &pkt, 0);
    if (ret != ESP_OK) { remove_fd(fd); return ret; }
    // Every frame that reaches this handler is read to its end: a payload left
    // in the stream is taken for the next frame's header (review 2026-10-24
    // HS-12). All frames come here, control frames too (handle_ws_control_frames,
    // below), so each is read under the frame deadline. Only text is handed on.
    if (pkt.type == HTTPD_WS_TYPE_PING || pkt.type == HTTPD_WS_TYPE_PONG || pkt.type == HTTPD_WS_TYPE_CLOSE) {
        uint8_t ctl[125];   // a control frame carries at most 125 bytes (RFC 6455 5.5)
        if (pkt.len > sizeof(ctl)) { remove_fd(fd); return ESP_FAIL; }
        pkt.payload = ctl;
        if (pkt.len && (ret = httpd_ws_recv_frame(req, &pkt, pkt.len)) != ESP_OK) { remove_fd(fd); return ret; }
        // Answered as httpd itself would (RFC 6455 5.5.1/5.5.2): a PING with a
        // PONG carrying its payload, a CLOSE with an empty CLOSE, after which
        // httpd ends the session (it set ws_close on reading the opcode) and our
        // close_fn cleans the table. A PONG needs nothing.
        if (pkt.type == HTTPD_WS_TYPE_PING) {
            pkt.type = HTTPD_WS_TYPE_PONG;
            return send_control(fd, &pkt);
        }
        if (pkt.type == HTTPD_WS_TYPE_CLOSE) {
            httpd_ws_frame_t close_frame{};
            close_frame.final = true;
            close_frame.type  = HTTPD_WS_TYPE_CLOSE;
            (void)send_control(fd, &close_frame);
        }
        return ESP_OK;
    }
    if (pkt.len > WS_RX_MAX) {
        // A failed handler makes httpd close the session; reading the payload
        // away instead would hold the httpd task as long as the client cares
        // to trickle it.
        ESP_LOGW(TAG, "ws frame %u B exceeds cap %u — closing the session",
                 (unsigned)pkt.len, (unsigned)WS_RX_MAX);
        remove_fd(fd);
        return ESP_FAIL;
    }
    if (pkt.len == 0) return ESP_OK;
    uint8_t* buf = static_cast<uint8_t*>(
        heap_caps_malloc(pkt.len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    // PSRAM short: internal RAM stands in only for a frame of up to WS_RX_INTERNAL_MAX, the cap before the 32 KB
    // one. A few frames of 32 KB from internal RAM would starve the radio and lwIP; a larger one fails its session.
    // MALLOC_CAP_DEFAULT, as malloc asks: without it the driver reserve (SPIRAM_MALLOC_RESERVE_INTERNAL) could serve it.
    if (!buf && pkt.len <= WS_RX_INTERNAL_MAX)
        buf = static_cast<uint8_t*>(heap_caps_malloc(pkt.len + 1, MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL));
    if (!buf) return ESP_ERR_NO_MEM;
    pkt.payload = buf;
    ret = httpd_ws_recv_frame(req, &pkt, pkt.len);
    if (ret == ESP_OK && pkt.type == HTTPD_WS_TYPE_TEXT && s_rx_cb) {
        buf[pkt.len] = '\0';
        s_rx_cb(fd, reinterpret_cast<const char*>(buf), pkt.len);
    }
    free(buf);
    return ret;
}

static const httpd_uri_t s_ws_uri = {
    .uri                      = "/ws",
    .method                   = HTTP_GET,
    .handler                  = ws_handler,
    .user_ctx                 = nullptr,
    .is_websocket             = true,
    // true → PING, PONG and CLOSE come to ws_handler, which answers them (an
    // older `true` swallowed PING without a PONG, so keepalive-probing clients
    // timed out). With false httpd read them itself, outside the frame
    // deadline: one trickled PING held the httpd task ~6 min (review R-04).
    // Session teardown after CLOSE runs through close_fn below.
    .handle_ws_control_frames = true,
    .supported_subprotocol    = nullptr,
#ifdef CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT
    .ws_pre_handshake_cb      = ws_pre_handshake,
#endif
#ifdef CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT
    // Field exists only on IDF >= 6.1 and only when the option is enabled;
    // both the struct member and the callback are compiled out otherwise, so
    // this stays source-compatible with v6.0.
    .ws_post_handshake_cb     = ws_post_handshake,
#endif
};

// Registered as httpd_config_t::close_fn — fires on EVERY terminated session
// (plain HTTP and WS alike; graceful CLOSE, TCP RST, keepalive death, LRU
// purge). Without it an abnormal disconnect never reached remove_fd and the
// 3-slot client table leaked until a later broadcast send happened to fail.
// remove_fd() is a no-op for fds that never joined the WS table, so plain
// HTTP sockets pass straight through. With a custom close_fn installed the
// server no longer close()s the socket itself — we must do it here.
// ── Request deadline (review 2026-10-24 HS-03) ───────────────────────────
// One httpd task serves every request in turn, and the receive timeout
// bounds only each read: a client trickling a byte per timeout held it, and
// every other request and WS frame behind it, for as long as it liked. Each
// HTTP request now has kRequestDeadlineUs from its first byte to its answer;
// past that its reads fail and httpd closes the session. The clock starts at
// a request's first read and stops when the hub sends anything back; a session
// upgraded to WebSocket has no request clock, but each of its frames gets the
// same deadline (ws_frame_clock, R-04). All of
// this runs on the httpd task (reads, answers, open and close), so the table
// needs no lock; WS frames sent from other tasks leave it alone.
static constexpr int64_t kRequestDeadlineUs = 8 * 1000 * 1000;
// fd 0 = free slot; start 0 = no request running; frame 0 = no WS frame being read
struct ReqClock { int fd; int64_t start_us; int64_t frame_us; };
static ReqClock s_clock[16];                     // >= max_open_sockets

static ReqClock* clock_of(int fd, bool add) {
    ReqClock* free_slot = nullptr;
    for (auto& c : s_clock) {
        if (c.fd == fd) return &c;
        if (!c.fd && !free_slot) free_slot = &c;
    }
    if (add && free_slot) { *free_slot = ReqClock{fd, 0, 0}; return free_slot; }
    return nullptr;
}

// Review 2026-10-28 R-04: a WebSocket frame gets the same deadline, from the
// start of its read in ws_handler to its end. The exemption above let a client
// trickle a byte per receive timeout through a whole frame (8 KB then, about
// 7 h; WS_RX_MAX, 32 KB, would be more than a day) with the httpd task held,
// and the sign-in sweep's close only queued behind it.
static void ws_frame_clock(int fd, bool start) {
    if (ReqClock* c = clock_of(fd, start)) c->frame_us = start ? esp_timer_get_time() : 0;
}

// httpd's own socket errors (IDF httpd_txrx.c httpd_sock_err).
static int sock_err() {
    switch (errno) {
    case EAGAIN: case EINTR:                      return HTTPD_SOCK_ERR_TIMEOUT;
    case EINVAL: case EBADF: case EFAULT: case ENOTSOCK: return HTTPD_SOCK_ERR_INVALID;
    default:                                      return HTTPD_SOCK_ERR_FAIL;
    }
}

static bool is_ws_session(httpd_handle_t hd, int fd) {
    return httpd_ws_get_fd_info(hd, fd) == HTTPD_WS_CLIENT_WEBSOCKET;
}

static int deadline_recv(httpd_handle_t hd, int fd, char* buf, size_t len, int flags) {
    if (!buf) return HTTPD_SOCK_ERR_INVALID;
    if (!is_ws_session(hd, fd)) {
        if (ReqClock* c = clock_of(fd, true)) {
            const int64_t now = esp_timer_get_time();
            if (!c->start_us) c->start_us = now;
            else if (now - c->start_us > kRequestDeadlineUs) return HTTPD_SOCK_ERR_FAIL;
        }
    } else if (ReqClock* c = clock_of(fd, false);
               c && c->frame_us && esp_timer_get_time() - c->frame_us > kRequestDeadlineUs) {
        return HTTPD_SOCK_ERR_FAIL;   // the frame ran past its deadline (R-04): httpd ends the session
    }
    const int r = recv(fd, buf, len, flags);
    return r < 0 ? sock_err() : r;
}

static int clock_send(httpd_handle_t hd, int fd, const char* buf, size_t len, int flags) {
    if (!buf) return HTTPD_SOCK_ERR_INVALID;
    if (!is_ws_session(hd, fd))
        if (ReqClock* c = clock_of(fd, false)) c->start_us = 0;   // answered: the next request starts afresh
    const int r = send(fd, buf, len, flags);
    return r < 0 ? sock_err() : r;
}

static esp_err_t ws_httpd_open_fn(httpd_handle_t hd, int sockfd) {
    clock_of(sockfd, true);
    httpd_sess_set_recv_override(hd, sockfd, deadline_recv);
    httpd_sess_set_send_override(hd, sockfd, clock_send);
    return ESP_OK;
}

static void ws_httpd_close_fn(httpd_handle_t hd, int sockfd) {
    (void)hd;
    remove_fd(sockfd);
    if (ReqClock* c = clock_of(sockfd, false)) *c = ReqClock{};
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_closing_n; i++) {
        if (s_closing[i] != sockfd) continue;
        for (int j = i + 1; j < s_closing_n; j++) s_closing[j - 1] = s_closing[j];
        s_closing_n--;
        break;
    }
    xSemaphoreGive(s_mutex);
    close(sockfd);
}

void ws_server_init() {
    s_mutex = xSemaphoreCreateMutex();
    configASSERT(s_mutex);
    s_tx_mutex = xSemaphoreCreateMutex();
    configASSERT(s_tx_mutex);

    httpd_config_t cfg      = HTTPD_DEFAULT_CONFIG();
    cfg.server_port         = 80;
    // Cap is CONFIG_LWIP_MAX_SOCKETS (10). httpd keeps one listen socket
    // aside, so we can safely request up to 9. Use the whole pool — the
    // SPA's editor chunk drags in several lazy imports; parallel fetches
    // from mobile browsers (up to 6) plus the persistent /ws connection
    // easily outruns a smaller pool.
    cfg.max_open_sockets    = 9;
    // Every REST route in rest_ops.cpp's kRoutes[] is registered as its OWN
    // httpd handler, plus /ws and the SPA static/catch-all handler on top —
    // so this must stay >= kRoutes count + 2. It is NOT a soft limit:
    // httpd_register_uri_handler() simply refuses once the slots run out, and
    // because the SPA handler registers last, the visible symptom is the whole
    // Web UI 404ing ("Nothing matches the given URI") while the REST/WS API
    // keeps working — no build error, no crash, boot log shows only a single
    // `httpd_uri: no slots left for registering handler` warning. That is
    // exactly what happened when the RainMaker ops grew kRoutes 41 -> 48
    // against a hardcoded 48. rest_ops.cpp carries a static_assert tying the
    // route count to WS_SERVER_MAX_URI_HANDLERS so this fails at compile time
    // from now on; raise both together if the table grows past the headroom.
    cfg.max_uri_handlers    = WS_SERVER_MAX_URI_HANDLERS;
    cfg.uri_match_fn        = httpd_uri_match_wildcard;
    cfg.stack_size          = 12288; // 8 K was tight: api_rule_list -> hap_roundtrip ->
                                     // SPI exchange -> peer-dispatch BULK callback ->
                                     // ws_event_broadcast -> ws_server_broadcast ->
                                     // lwip_send chain trips the canary on a single
                                     // WS request. 12 K covers the worst observed
                                     // path (httpd thread tracing through master_send
                                     // and back into ws send) plus headroom for
                                     // future handler additions.
#if defined(CONFIG_SPIRAM_XIP_FROM_PSRAM) && defined(CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM)
    // Firmware runs from PSRAM (S31 wired build): the 12 K httpd stack lives
    // there too and stays off the tight internal DRAM. Other builds keep the
    // httpd default (internal).
    cfg.task_caps           = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
#endif
    // Mobile browsers (Safari/Chrome on iOS/Android) open up to 6 parallel
    // TCP connections to the same host for speculative fetches + asset
    // prefetch. LRU eviction re-purposes the oldest idle connection for
    // new accepts so bursts clear cleanly even if the pool fills.
    cfg.lru_purge_enable    = true;
    // TCP keepalive — without this, a half-open connection (mobile
    // browser tabbed away on a flaky link, NAT idle-timeout dropping
    // the path) is only detected when the next ws_broadcast fails
    // with ECONNRESET / ENOTCONN. Sending probes after 10 s of idle
    // catches the death within ~30 s and frees the socket pool slot.
    cfg.keep_alive_enable   = true;
    cfg.keep_alive_idle     = 10;
    cfg.keep_alive_interval = 5;
    cfg.keep_alive_count    = 3;
    // F33 (FINDINGS.md): bound slow / half-open clients (slowloris) so a few
    // trickle connections can't pin the small (9) socket pool indefinitely.
    // 3 s per read, and kRequestDeadlineUs per request (HS-03, open_fn).
    cfg.recv_wait_timeout   = 3;
    cfg.send_wait_timeout   = 10;
    cfg.open_fn             = ws_httpd_open_fn;
    // Slot cleanup on ANY socket close (see ws_httpd_close_fn above).
    cfg.close_fn            = ws_httpd_close_fn;

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return;
    }
    httpd_register_uri_handler(s_server, &s_ws_uri);
    // HS-04: the sign-in deadline for unsigned sockets.
    static esp_timer_handle_t sweep = nullptr;
    const esp_timer_create_args_t args = {
        .callback = [](void*) { ws_server_sign_in_sweep(esp_timer_get_time()); },
        .arg = nullptr, .dispatch_method = ESP_TIMER_TASK, .name = "ws_signin", .skip_unhandled_events = true,
    };
    if (!sweep && esp_timer_create(&args, &sweep) == ESP_OK) esp_timer_start_periodic(sweep, 1000 * 1000);
    ESP_LOGI(TAG, "ws_server started on :80/ws");
}

httpd_handle_t ws_server_get_handle() {
    return s_server;
}

void ws_server_set_rx_callback(WsRxCallback cb) {
    s_rx_cb = cb;
}

void ws_server_set_upgrade_filter(WsUpgradeFilter fn) {
    s_upgrade_filter = fn;
}

void ws_server_set_api_token(const char* token) {
    strncpy(s_api_token, token ? token : "", sizeof(s_api_token) - 1);
    s_api_token[sizeof(s_api_token) - 1] = '\0';
}

void ws_server_register_reply_hook(int sentinel_fd, WsReplyHook hook) {
    // No mutex needed: registration happens once at init, before
    // any traffic reaches ws_server_reply.
    s_hook_fd   = sentinel_fd;
    s_hook_func = hook;
}

int ws_server_client_count() {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int n = s_fd_count;
    xSemaphoreGive(s_mutex);
    return n;
}

// Task currently inside send_locked()'s send loop (a broadcast fan-out or a
// point reply), nullptr when none.
// The log pipeline checks this (ws_server_in_broadcast) and drops its WS-sink
// fan-out for lines logged from within the send path — defence-in-depth
// against TX-path re-entry (e.g. esp_http_server's own ESP_LOG* inside
// httpd_ws_send_frame_async). Written under s_tx_mutex: net-core broadcasts
// from one TX worker, but the wired and single-chip builds broadcast from
// several tasks (event bus, status tick, log sink), so the send loop is
// serialized rather than asserted single-writer.
static std::atomic<TaskHandle_t> s_broadcast_task{nullptr};

bool ws_server_in_broadcast() {
    return s_broadcast_task.load(std::memory_order_acquire) ==
           xTaskGetCurrentTaskHandle();
}

// Broadcast only (see send_locked, `recheck`): is this fd still a registered
// client, and signed in when sign-in is on? Takes s_mutex while the caller holds
// s_tx_mutex. That is the only lock order there is: s_mutex is a leaf lock
// (nothing is called, and nothing logged, while it is held), so no task ever
// takes s_tx_mutex while holding it.
static bool fd_may_receive(int fd) {
    bool ok = false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_fd_count; i++) {
        // The socket's own flag: one opened while no token is set starts signed
        // in, and one that never signed in gets nothing, token or not (HS-01).
        if (s_fds[i] == fd) { ok = s_fd_authed[i]; break; }
    }
    xSemaphoreGive(s_mutex);
    return ok;
}

// The ONLY place that writes frames to client sockets, and the only place that
// handles their failure. httpd_ws_send_frame_async sends a frame as TWO send()s
// (header, then payload) with no lock of its own, so two tasks writing one socket
// interleave them: one frame's header lands inside the other's payload and the
// browser fails the connection (close 1006). Every writer — broadcast fan-out and
// point replies alike — therefore takes turns on s_tx_mutex and sets the
// s_broadcast_task marker, so a log line emitted from inside the send is dropped
// by the log sink instead of re-entering ws_server_broadcast on this task and
// deadlocking on the mutex. Sends `pkt` to fds[0..n), n <= MAX_WS_CLIENTS. If the
// lock was not taken nothing is sent and nothing is evicted: those fds are
// healthy (with portMAX_DELAY it cannot happen anyway).
//
// `recheck` (broadcast only): the fd list was snapshotted BEFORE the lock was
// taken, and this task may have waited a long time for it (behind a reply that
// is draining to a slow client, or a send stalled on a dead one). By then httpd
// can have closed an fd and handed its number to a new connection: an HTTP fetch
// (the frame would land inside its response) or a WS client that has not signed
// in yet (it would get what the auth gate withholds). So each fd is checked again
// once the lock is held, and one that fails is skipped. Replies are never
// re-checked: their target is the requester, signed in or not.
//
// A fd whose send FAILED is a dead or stalled client. It
//  1. leaves the table at once, WHILE s_tx_mutex IS STILL HELD. A waiter that
//     outranks this task (httpd, priority 5, behind TaskEventBus, 2) takes the
//     lock at the give and runs before this task executes another line: with the
//     eviction after the give, its re-check would still find the dead fd and it
//     would stall on it a second time (up to WS_SEND_TIMEOUT_S more, with the
//     whole web server behind it). The removal is quiet: nothing is logged under
//     either lock, because the wired/single-chip log sinks broadcast from inside
//     the log call;
//  2. has its session closed after the lock is released, but only if THIS call
//     removed it. Dropping the fd alone leaves the TCP socket open as far as httpd
//     is concerned; the browser's next frame reaches ws_handler, ensure_fd()
//     re-adopts it with authed=0, and with sign-in on every command then answers
//     "auth required" until the page reloads. Closed, the page reconnects (1 s)
//     and signs in again. httpd queues a close by session POINTER, so a second
//     close for the same session (another sender with a stale fd snapshot, a reply
//     after a broadcast) could close whatever new connection has since taken that
//     slot. The close is queued to the httpd task (httpd_queue_work), so any task
//     may request it, httpd itself included. Best effort, like adopt_client: the
//     result is ignored (a full httpd control queue leaves the old behaviour for
//     this fd);
//  3. is logged last, so a log line emitted from httpd_sess_trigger_close, or the
//     warning itself, can never be broadcast back to the dead fd. `who` names the
//     sender in the warning.
// Lock order: s_tx_mutex, then s_mutex (fd_may_receive, remove_fd_quiet), and
// never the other way round: s_mutex is a leaf lock — nothing is called while it
// is held.
//
// Also the hub page's UTF-8 guard (utf8_seq.h): Chrome closes the socket on one
// invalid byte in a text frame, so a frame with any is sent from a copy with '?'
// for each such byte (same length; JSON's structural bytes are ASCII). The
// caller's buffer is left alone, and a frame that cannot be copied is dropped
// rather than sent invalid. The copy comes from PSRAM; with PSRAM short, internal
// RAM stands in only for a frame of up to WS_RX_INTERNAL_MAX, and only the RAM
// malloc takes (MALLOC_CAP_DEFAULT): a few large frames would starve the radio and
// lwIP. Silent on purpose: a log line from here would come back through the wired
// log sink.
static void send_locked(const int* fds, int n, httpd_ws_frame_t* pkt, bool recheck, const char* who) {
    char* fixed = nullptr;
    if (!utf8_valid(reinterpret_cast<const char*>(pkt->payload), pkt->len)) {
        fixed = static_cast<char*>(heap_caps_malloc(pkt->len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        // PSRAM short: the capped internal-RAM fallback described above.
        if (!fixed && pkt->len <= WS_RX_INTERNAL_MAX)
            fixed = static_cast<char*>(heap_caps_malloc(pkt->len, MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL));
        if (!fixed) return;
        memcpy(fixed, pkt->payload, pkt->len);
        utf8_scrub(fixed, pkt->len);
        pkt->payload = reinterpret_cast<uint8_t*>(fixed);
    }
    esp_err_t err[MAX_WS_CLIENTS];
    int       left[MAX_WS_CLIENTS];   // table size after we removed that fd; -1: it did not fail, or was already gone
    if (xSemaphoreTake(s_tx_mutex, portMAX_DELAY) != pdTRUE) { free(fixed); return; }
    s_broadcast_task.store(xTaskGetCurrentTaskHandle(), std::memory_order_release);
    // NOTE: no early returns between the guard stores — in_broadcast must clear.
    for (int i = 0; i < n; i++) {
        err[i]  = ESP_OK;
        left[i] = -1;
        if (recheck && !fd_may_receive(fds[i])) continue;
        err[i] = httpd_ws_send_frame_async(s_server, fds[i], pkt);
        if (err[i] != ESP_OK) left[i] = remove_fd_quiet(fds[i]);
    }
    s_broadcast_task.store(nullptr, std::memory_order_release);
    xSemaphoreGive(s_tx_mutex);
    free(fixed);   // httpd_ws_send_frame_async has written the frame by the time it returns

    for (int i = 0; i < n; i++) {
        if (err[i] == ESP_OK) continue;
        if (left[i] >= 0) {
            httpd_sess_trigger_close(s_server, fds[i]);
            ESP_LOGI(TAG, "WS client removed fd=%d total=%d", fds[i], left[i]);
        }
        ESP_LOGW(TAG, "%s: send failed fd=%d err=%d (removed)", who, fds[i], err[i]);
    }
}

// A control frame's answer (ws_handler's PONG or CLOSE) takes its turn on
// s_tx_mutex like every other frame: a broadcast to the same socket would
// otherwise interleave its two send()s with ours (review 2026-10-28, fix
// review N1). Not through send_locked: a PONG must echo the PING's bytes, which
// need not be UTF-8, and a failure here is handled by httpd ending the session.
static esp_err_t send_control(int fd, httpd_ws_frame_t* pkt) {
    if (xSemaphoreTake(s_tx_mutex, portMAX_DELAY) != pdTRUE) return ESP_FAIL;
    s_broadcast_task.store(xTaskGetCurrentTaskHandle(), std::memory_order_release);
    const esp_err_t err = httpd_ws_send_frame_async(s_server, fd, pkt);
    s_broadcast_task.store(nullptr, std::memory_order_release);
    xSemaphoreGive(s_tx_mutex);
    return err;
}

void ws_server_broadcast(const char* json, size_t len) {
    if (!s_server || !s_mutex || !s_tx_mutex) return;
    // A line logged from inside our own send loop would come back here on the
    // same task; the log sink drops it already, and taking s_tx_mutex again
    // would deadlock.
    if (ws_server_in_broadcast()) return;

    // Snapshot fd list outside the send loop so we don't hold the mutex
    // while calling potentially blocking httpd API.
    //
    // Auth gate: when a token is configured (token set ⇔ auth enabled — main
    // clears it via ws_server_set_api_token(nullptr) on disable), sockets that
    // opened /ws but never authenticated must NOT passively receive events
    // (device states, alerts, log lines). The auth handshake reply itself is a
    // targeted ws_server_reply(), never a broadcast, so gating here cannot
    // break the {"cmd":"auth"} exchange.
    int  snap_fd[MAX_WS_CLIENTS];
    bool snap_authed[MAX_WS_CLIENTS];
    int  snap_n = 0;
    const bool auth_on = (s_api_token[0] != '\0');
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_fd_count; i++) {
        snap_authed[snap_n] = s_fd_authed[i];
        snap_fd[snap_n++]   = s_fds[i];
    }
    xSemaphoreGive(s_mutex);

    // Drop entries httpd no longer considers live WebSocket sessions.
    //
    // The failure-driven cleanup further down only fires when a send actually
    // errors, and a half-open TCP socket keeps ACCEPTING writes long after the
    // peer is gone — so an abruptly-killed client could hold a slot
    // indefinitely. With MAX_WS_CLIENTS at 3 that is two dead browsers away
    // from refusing every new connection. httpd_ws_get_fd_info() is the
    // authoritative answer and costs a table lookup.
    //
    // Deliberately outside the mutex: this file's rule is never to call httpd
    // while holding it, and remove_fd() takes it itself.
    int  fds[MAX_WS_CLIENTS] = {};   // zero-init: send_locked() takes it as const int*
    int  count = 0;
    for (int i = 0; i < snap_n; i++) {
        if (httpd_ws_get_fd_info(s_server, snap_fd[i]) != HTTPD_WS_CLIENT_WEBSOCKET) {
            remove_fd(snap_fd[i]);
            continue;
        }
        if (auth_on && !snap_authed[i]) continue;
        fds[count++] = snap_fd[i];
    }
    // Nobody to send to: do not queue behind the TX lock. With the wired/mono WS
    // log sink on, every log line from every task is a broadcast, and most of
    // them have no client signed in; they would all wait behind a reply that is
    // draining to a slow client.
    if (count == 0) return;

    httpd_ws_frame_t pkt{};
    pkt.type    = HTTPD_WS_TYPE_TEXT;
    pkt.payload = reinterpret_cast<uint8_t*>(const_cast<char*>(json));
    pkt.len     = len;

    // Two tasks broadcasting at once tripped the old single-writer assert and
    // rebooted the wired hub (main's status tick vs the event bus). Take turns.
    // send_locked also evicts a failed fd, closes its session and logs the failure,
    // in that order and outside the locks: a failure log used to recurse (log →
    // sink → broadcast → fail → log → …) until stack overflow.
    send_locked(fds, count, &pkt, true, "ws_broadcast");
}

void ws_server_reply(int fd, const char* json, size_t len) {
    // Sentinel-fd fast path: route the reply to a registered hook
    // (e.g. remote_client) instead of the local httpd send. The
    // sentinel is chosen outside the legal httpd fd range.
    if (s_hook_func && fd == s_hook_fd) {
        s_hook_func(json, len);
        return;
    }
    if (!s_server || fd < 0) return;
    // A reply issued from inside our own send (the marker is set on this task,
    // e.g. from a log hook) would take s_tx_mutex a second time: a silent
    // self-deadlock. Fail loudly instead. Below the sentinel fast path on
    // purpose: that route never takes the lock, so it is safe there.
    configASSERT(!ws_server_in_broadcast());
    httpd_ws_frame_t pkt{};
    pkt.type    = HTTPD_WS_TYPE_TEXT;
    pkt.payload = reinterpret_cast<uint8_t*>(const_cast<char*>(json));
    pkt.len     = len;
    // Same turn-taking as the broadcast loop (see send_locked): a reply runs on
    // the httpd task while another task may be broadcasting to this socket. A
    // failed send is handled there too (evict, close, log).
    send_locked(&fd, 1, &pkt, false, "ws_reply");
}
