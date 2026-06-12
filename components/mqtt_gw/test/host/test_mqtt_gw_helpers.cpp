// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host tests for mqtt_gw_s3.cpp T19 PURE security helpers.
//
// mqtt_gw_s3.cpp pulls in esp-mqtt + esp_crt_bundle + FreeRTOS + esp_timer
// + metrics, so it cannot link on the host, and a whole-TU #include would
// require stubbing the entire esp_mqtt_client_* API. The two functions
// under test are file-local `static` and otherwise PURE (only <cstring> /
// <cstdio>), so — matching the zap_store/test/host precedent — we MIRROR
// their EXACT logic below and keep it in lockstep with the source.
//
//   F3-T19 mqtt_topic_ok:   a PUBLISH topic name must not carry MQTT
//                           wildcards (+/#), control bytes (<0x20 / 0x7f),
//                           or run >= 159 bytes (fmt_topic buffer is 160).
//   F4-T19 redact_userinfo: drop any user:pass@ between "://" and the
//                           authority so credentials never reach the log.
#include <cstdio>
#include <cstring>
#include <cstddef>

static int s_failures = 0;
#define CHECK(cond, msg) do {                                              \
    if (!(cond)) { s_failures++; printf("  FAIL: %s\n", msg); }            \
    else         { printf("  ok:   %s\n", msg); }                          \
} while (0)

// ── mirror of mqtt_gw_s3.cpp:mqtt_topic_ok — keep in sync ─────────────────
static bool mqtt_topic_ok(const char* t) {
    if (!t || !t[0]) return false;
    size_t n = 0;
    for (const unsigned char* p = (const unsigned char*)t; *p; ++p, ++n) {
        if (n >= 159) return false;            // overlong (fmt_topic is 160)
        if (*p == '+' || *p == '#') return false;
        if (*p < 0x20 || *p == 0x7f) return false;  // control / DEL
    }
    return n > 0;
}

// ── mirror of mqtt_gw_s3.cpp:redact_userinfo — keep in sync ───────────────
// (uses a static buffer exactly as the source does; callers use it inline.)
static const char* redact_userinfo(const char* url) {
    static char buf[128];
    if (!url || !url[0]) { buf[0] = '\0'; return buf; }
    const char* p = strstr(url, "://");
    if (!p) {
        strncpy(buf, url, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        return buf;
    }
    const char* scheme_end = p + 3;
    const char* path = strchr(scheme_end, '/');
    const char* at   = nullptr;
    for (const char* q = scheme_end; *q && (!path || q < path); ++q) {
        if (*q == '@') at = q;
    }
    const char* host = at ? at + 1 : scheme_end;
    int n = snprintf(buf, sizeof(buf), "%.*s%s",
                     (int)(scheme_end - url), url, host);
    if (n < 0) buf[0] = '\0';
    return buf;
}

int main() {
    printf("test_mqtt_gw_helpers (T19 F3 topic / F4 url redaction)\n");

    // ── F3-T19: mqtt_topic_ok ────────────────────────────────────────────
    {
        // accept: ordinary topics
        CHECK(mqtt_topic_ok("zhac/sensor/state"), "accept normal topic");
        CHECK(mqtt_topic_ok("a"), "accept single-char topic");
        CHECK(mqtt_topic_ok("zhac/0102/availability"), "accept hierarchical");
        // reject: wildcards (illegal in a PUBLISH topic name)
        CHECK(!mqtt_topic_ok("zhac/+/state"), "reject '+' wildcard");
        CHECK(!mqtt_topic_ok("zhac/#"), "reject '#' wildcard");
        CHECK(!mqtt_topic_ok("+"), "reject bare '+'");
        // reject: control bytes / DEL
        CHECK(!mqtt_topic_ok("zhac/\x01/x"), "reject control byte <0x20");
        CHECK(!mqtt_topic_ok("zhac/\x1f/x"), "reject 0x1f");
        CHECK(!mqtt_topic_ok("zhac/\x7f/x"), "reject 0x7f DEL");
        // reject: empty / null
        CHECK(!mqtt_topic_ok(""), "reject empty");
        CHECK(!mqtt_topic_ok(nullptr), "reject null");
        // boundary: the guard is `if (n >= 159) return false` evaluated at
        // the START of each iteration, so it only fires once a char sits at
        // index n==159 — i.e. the 160th char. A 159-char topic (last char at
        // n==158) is therefore ACCEPTED; a 160-char topic is REJECTED.
        char t159[160]; memset(t159, 'a', 159); t159[159] = '\0';
        CHECK(mqtt_topic_ok(t159), "accept 159-char topic (boundary)");
        char t160[161]; memset(t160, 'a', 160); t160[160] = '\0';
        CHECK(!mqtt_topic_ok(t160), "reject 160-char topic (overlong)");
        // high bytes (>=0x80, e.g. UTF-8) are NOT control bytes → accepted.
        CHECK(mqtt_topic_ok("zhac/\xc3\xa9/x"), "accept UTF-8 high bytes");
    }

    // ── F4-T19: redact_userinfo ───────────────────────────────────────────
    {
        // credentials present → stripped, scheme + host:port kept
        CHECK(strcmp(redact_userinfo("mqtt://user:pass@host:1883"),
                     "mqtt://host:1883") == 0,
              "strip user:pass@ -> scheme://host:port");
        CHECK(strcmp(redact_userinfo("mqtts://u:p@broker.example.com:8883"),
                     "mqtts://broker.example.com:8883") == 0,
              "strip on mqtts://");
        // no '@' → unchanged
        CHECK(strcmp(redact_userinfo("mqtt://host:1883"),
                     "mqtt://host:1883") == 0,
              "no '@' unchanged");
        // '@' only in the path (after the first '/') → authority preserved,
        // path kept (the cut is bounded to before the path).
        CHECK(strcmp(redact_userinfo("mqtt://host:1883/topic@x"),
                     "mqtt://host:1883/topic@x") == 0,
              "'@' in path preserved");
        // username only (no password)
        CHECK(strcmp(redact_userinfo("mqtt://user@host:1883"),
                     "mqtt://host:1883") == 0,
              "strip username-only userinfo");
        // no scheme → returned as-is (defensive copy)
        CHECK(strcmp(redact_userinfo("just-a-host"), "just-a-host") == 0,
              "no scheme returned verbatim");
        // empty / null
        CHECK(redact_userinfo("")[0] == '\0', "empty -> empty");
        CHECK(redact_userinfo(nullptr)[0] == '\0', "null -> empty");
    }

    if (s_failures) { printf("FAILED: %d check(s)\n", s_failures); return 1; }
    printf("ALL PASS\n");
    return 0;
}
