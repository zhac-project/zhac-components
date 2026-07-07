// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Characterization matrix for mqtt_gw_s3.cpp's PURE topic/config logic.
//
// Supplements test_mqtt_gw_helpers.cpp (which pins the two T19 security
// helpers mqtt_topic_ok + redact_userinfo). This file covers the rest of
// the host-reachable logic:
//
//   - root-topic module (public API):
//       mqtt_gw_set_root_topic  — clear / trailing-slash trim / truncate
//       mqtt_gw_get_root_topic  — "" → default "zhac"
//       mqtt_gw_format_topic    — "<root>/<suffix>" join, leading-'/' strip,
//                                 overflow (-1), null/empty-cap guards
//   - mqtt_gw_is_secure          — verified-TLS scheme gate (mqtts:// / wss://)
//   - mqtt_gw_publish prefixing  — F3-T19 routing: leading-'/' absolute,
//                                 already-root-prefixed pass-through, otherwise
//                                 prepend "<root>/", overflow-drop; + F23 QoS
//                                 clamp to 0..2
//
// Same rationale + method as the helpers test: mqtt_gw_s3.cpp links esp-mqtt
// + esp_crt_bundle + FreeRTOS + esp_timer + metrics and cannot build on the
// host, and a whole-TU #include would need the entire esp_mqtt_client_* stub
// surface. So — matching the zap_store/test/host precedent — we MIRROR the
// EXACT source logic below (each block marked "mirror of ... — keep in sync",
// including the s_root_topic[32]/s_broker_url[128] state it reads) and pin it
// as a regression. If the source drifts, this is the canary.
#include <cstdio>
#include <cstring>
#include <cstddef>
#include <cstdint>

static int s_failures = 0;
#define CHECK(cond, msg) do {                                              \
    if (!(cond)) { s_failures++; printf("  FAIL: %s\n", msg); }            \
    else         { printf("  ok:   %s\n", msg); }                          \
} while (0)

// ── mirror of mqtt_gw_s3.cpp config state (the fields the logic reads) ─────
static char s_root_topic[32]  = "zhac";   // default; set_root_topic("") clears
static char s_broker_url[128] = {};

// ── mirror of mqtt_gw_s3.cpp:mqtt_gw_set_root_topic — keep in sync ─────────
static void set_root_topic(const char* root) {
    if (!root || !root[0]) { s_root_topic[0] = '\0'; return; }
    strncpy(s_root_topic, root, sizeof(s_root_topic) - 1);
    s_root_topic[sizeof(s_root_topic) - 1] = '\0';
    // Trim ONE trailing slash if present — format_topic always inserts one.
    size_t n = strlen(s_root_topic);
    if (n && s_root_topic[n - 1] == '/') s_root_topic[n - 1] = '\0';
}

// ── mirror of mqtt_gw_s3.cpp:mqtt_gw_get_root_topic — keep in sync ─────────
static const char* get_root_topic(void) {
    return s_root_topic[0] ? s_root_topic : "zhac";
}

// ── mirror of mqtt_gw_s3.cpp:mqtt_gw_format_topic — keep in sync ───────────
static int format_topic(char* out, size_t cap, const char* suffix) {
    if (!out || cap == 0) return -1;
    const char* root = s_root_topic[0] ? s_root_topic : "zhac";
    const char* sfx  = suffix ? suffix : "";
    if (sfx[0] == '/') sfx++;   // avoid `<root>//suffix` (strips exactly one)
    int n = snprintf(out, cap, "%s/%s", root, sfx);
    if (n < 0 || (size_t)n >= cap) return -1;
    return n;
}

// ── mirror of mqtt_gw_s3.cpp:mqtt_gw_is_secure — keep in sync ──────────────
static bool is_secure() {
    return strncmp(s_broker_url, "mqtts://", 8) == 0 ||
           strncmp(s_broker_url, "wss://",   6) == 0;
}

// ── mirror of mqtt_gw_s3.cpp:mqtt_topic_ok — keep in sync ──────────────────
// Standalone coverage lives in test_mqtt_gw_helpers.cpp; re-mirrored here
// only to exercise the publish-path integration (prefix THEN validate).
static bool mqtt_topic_ok(const char* t) {
    if (!t || !t[0]) return false;
    size_t n = 0;
    for (const unsigned char* p = (const unsigned char*)t; *p; ++p, ++n) {
        if (n >= 159) return false;
        if (*p == '+' || *p == '#') return false;
        if (*p < 0x20 || *p == 0x7f) return false;
    }
    return n > 0;
}

// ── mirror of the topic-prefixing stage inside mqtt_gw_s3.cpp:mqtt_gw_publish
// (up to the point `eff` is finalized) — keep in sync ─────────────────────
// The source computes `eff` as a bare pointer into topic / topic+1 / a local
// fmt_topic[160], then validates mqtt_topic_ok(eff). We copy `eff` into `out`
// (cap must be >= 160) so the value survives return; behavior is otherwise
// identical. Returns strlen(eff), or -1 on the F3-T19 overflow-drop.
static int publish_effective_topic(const char* topic, char* out, size_t cap) {
    const char* eff = topic;
    char fmt_topic[160];
    if (topic[0] == '/') {
        eff = topic + 1;               // absolute — strip slash, no root
    } else if (s_root_topic[0]) {
        const size_t rlen = strlen(s_root_topic);
        if (strncmp(topic, s_root_topic, rlen) == 0 && topic[rlen] == '/') {
            eff = topic;               // already root-prefixed — as-is
        } else {
            int n = snprintf(fmt_topic, sizeof(fmt_topic), "%s/%s",
                             s_root_topic, topic);
            if (n > 0 && (size_t)n < sizeof(fmt_topic)) {
                eff = fmt_topic;       // prepend "<root>/"
            } else {
                return -1;             // F3-T19 overflow-drop
            }
        }
    }
    strncpy(out, eff, cap - 1);
    out[cap - 1] = '\0';
    return (int)strlen(out);
}

// ── mirror of mqtt_gw_s3.cpp:mqtt_gw_publish F23 QoS clamp — keep in sync ──
static uint8_t clamp_qos(int qos) {
    return (uint8_t)(qos < 0 ? 0 : (qos > 2 ? 2 : qos));
}

// Test helper: store a broker URL exactly as mqtt_gw_set_broker_url's storage
// step does (strncpy into s_broker_url[128]); is_secure() reads this field.
static void put_broker_url(const char* u) {
    if (!u) { s_broker_url[0] = '\0'; return; }
    strncpy(s_broker_url, u, sizeof(s_broker_url) - 1);
    s_broker_url[sizeof(s_broker_url) - 1] = '\0';
}

int main() {
    printf("test_mqtt_gw_matrix (root-topic / secure-gate / publish routing)\n");

    // ── A: get/set root topic ─────────────────────────────────────────────
    {
        // initial static state is "zhac" before any set
        CHECK(strcmp(get_root_topic(), "zhac") == 0, "default root is 'zhac'");
        set_root_topic("home");
        CHECK(strcmp(get_root_topic(), "home") == 0, "set simple root");
        set_root_topic("home/zhac-kitchen");
        CHECK(strcmp(get_root_topic(), "home/zhac-kitchen") == 0,
              "set hierarchical root (two controllers coexist)");
        // trailing slash trimmed (exactly one)
        set_root_topic("home/");
        CHECK(strcmp(get_root_topic(), "home") == 0, "trim trailing slash");
        set_root_topic("a/b//");
        CHECK(strcmp(get_root_topic(), "a/b/") == 0,
              "trim only ONE trailing slash (a/b// -> a/b/)");
        // null / empty clear the field → get falls back to "zhac"
        set_root_topic(nullptr);
        CHECK(strcmp(get_root_topic(), "zhac") == 0, "null clears -> default");
        set_root_topic("x");
        set_root_topic("");
        CHECK(strcmp(get_root_topic(), "zhac") == 0, "empty clears -> default");
        // over-long root truncated to 31 chars (buffer is 32)
        char longr[40]; memset(longr, 'a', 39); longr[39] = '\0';
        set_root_topic(longr);
        CHECK((int)strlen(get_root_topic()) == 31, "over-long root truncated to 31");
    }

    // ── B: format_topic ───────────────────────────────────────────────────
    {
        set_root_topic("zhac");   // reset to a known root for this block
        char buf[200];
        CHECK(format_topic(buf, sizeof(buf), "log/info") == 13 &&
              strcmp(buf, "zhac/log/info") == 0, "join root + suffix");
        // leading '/' on suffix stripped (exactly one) → no `zhac//log`
        CHECK(format_topic(buf, sizeof(buf), "/log/info") == 13 &&
              strcmp(buf, "zhac/log/info") == 0, "strip one leading '/' of suffix");
        // two leading slashes → only one stripped → double slash survives
        CHECK(format_topic(buf, sizeof(buf), "//x") == 7 &&
              strcmp(buf, "zhac//x") == 0, "only one leading '/' stripped (//x)");
        // empty / null suffix → "<root>/"
        CHECK(format_topic(buf, sizeof(buf), "") == 5 &&
              strcmp(buf, "zhac/") == 0, "empty suffix -> '<root>/'");
        CHECK(format_topic(buf, sizeof(buf), nullptr) == 5 &&
              strcmp(buf, "zhac/") == 0, "null suffix -> '<root>/'");
        // custom root honored
        set_root_topic("home");
        CHECK(format_topic(buf, sizeof(buf), "a") == 6 &&
              strcmp(buf, "home/a") == 0, "custom root honored");
        set_root_topic("zhac");
        // guards
        CHECK(format_topic(nullptr, 10, "x") == -1, "null out -> -1");
        CHECK(format_topic(buf, 0, "x") == -1, "cap 0 -> -1");
        // overflow: "zhac/x" needs 6 chars + NUL. cap==7 fits, cap==6 does not.
        CHECK(format_topic(buf, 7, "x") == 6, "exact-fit cap (7 for 'zhac/x')");
        CHECK(format_topic(buf, 6, "x") == -1, "off-by-one overflow -> -1");
        CHECK(format_topic(buf, 5, "log") == -1, "overflow -> -1");
    }

    // ── C: is_secure (verified-TLS scheme gate, F45) ──────────────────────
    {
        put_broker_url("");
        CHECK(!is_secure(), "empty url -> not secure");
        put_broker_url("mqtt://host:1883");
        CHECK(!is_secure(), "mqtt:// -> not secure");
        put_broker_url("ws://host/mqtt");
        CHECK(!is_secure(), "ws:// -> not secure");
        put_broker_url("mqtts://host:8883");
        CHECK(is_secure(), "mqtts:// -> secure");
        put_broker_url("wss://host/mqtt");
        CHECK(is_secure(), "wss:// -> secure");
        // near-miss schemes must NOT pass the gate
        put_broker_url("mqtts:/host");
        CHECK(!is_secure(), "single-slash 'mqtts:/' -> not secure");
        put_broker_url("MQTTS://host");
        CHECK(!is_secure(), "scheme match is case-sensitive");
    }

    // ── D: publish topic-prefixing / routing (F3-T19) ─────────────────────
    {
        set_root_topic("zhac");
        char eff[200];
        // not prefixed → prepend "<root>/"
        CHECK(publish_effective_topic("foo/bar", eff, sizeof(eff)) >= 0 &&
              strcmp(eff, "zhac/foo/bar") == 0, "prepend root when unprefixed");
        // leading '/' → absolute: strip slash, NO root
        CHECK(publish_effective_topic("/abs/topic", eff, sizeof(eff)) >= 0 &&
              strcmp(eff, "abs/topic") == 0, "leading '/' absolute (no root)");
        // already "<root>/..." → pass through unchanged (no double-prefix)
        CHECK(publish_effective_topic("zhac/already", eff, sizeof(eff)) >= 0 &&
              strcmp(eff, "zhac/already") == 0, "already-prefixed pass-through");
        // root as a bare substring (no '/' boundary) → still prepended
        CHECK(publish_effective_topic("zhacfoo", eff, sizeof(eff)) >= 0 &&
              strcmp(eff, "zhac/zhacfoo") == 0, "root substring w/o '/' -> prepend");
        // topic == root exactly (no trailing '/') → prepended
        CHECK(publish_effective_topic("zhac", eff, sizeof(eff)) >= 0 &&
              strcmp(eff, "zhac/zhac") == 0, "topic == root exactly -> prepend");
        // absolute beats root even when a root is set
        set_root_topic("home");
        CHECK(publish_effective_topic("/x/y", eff, sizeof(eff)) >= 0 &&
              strcmp(eff, "x/y") == 0, "absolute ignores configured root");
        set_root_topic("zhac");
        // empty root → no prefix branch fires → bare topic published
        set_root_topic("");
        CHECK(publish_effective_topic("foo", eff, sizeof(eff)) >= 0 &&
              strcmp(eff, "foo") == 0, "empty root -> bare topic (no prefix)");
        set_root_topic("zhac");
        // overflow-drop boundary: "zhac/" (5) + suffix; drop when total >= 160.
        // len==154 -> n==159 (accepted); len==155 -> n==160 (dropped).
        char t154[160]; memset(t154, 'a', 154); t154[154] = '\0';
        CHECK(publish_effective_topic(t154, eff, sizeof(eff)) == 159,
              "154-char topic prefixes to 159 (boundary accept)");
        char t155[160]; memset(t155, 'a', 155); t155[155] = '\0';
        CHECK(publish_effective_topic(t155, eff, sizeof(eff)) == -1,
              "155-char topic overflows fmt_topic -> drop (-1)");
    }

    // ── E: publish routing + mqtt_topic_ok integration ────────────────────
    {
        set_root_topic("zhac");
        char eff[200];
        // wildcard in an already-prefixed topic → prefixes fine, then dropped
        // by the post-prefix mqtt_topic_ok(eff) validation.
        CHECK(publish_effective_topic("zhac/+/x", eff, sizeof(eff)) >= 0 &&
              !mqtt_topic_ok(eff), "prefixed '+' passes prefix, fails validate");
        // '#' survives prepend, then rejected
        CHECK(publish_effective_topic("foo#", eff, sizeof(eff)) >= 0 &&
              strcmp(eff, "zhac/foo#") == 0 && !mqtt_topic_ok(eff),
              "'#' prepended then rejected");
        // bare "/" → absolute strips to "" → empty eff → validation drops it
        CHECK(publish_effective_topic("/", eff, sizeof(eff)) == 0 &&
              !mqtt_topic_ok(eff), "'/' -> empty eff -> validation drop");
        // a clean prefixed topic passes both stages
        CHECK(publish_effective_topic("sensor/state", eff, sizeof(eff)) >= 0 &&
              strcmp(eff, "zhac/sensor/state") == 0 && mqtt_topic_ok(eff),
              "clean topic passes prefix + validate");
    }

    // ── F: F23 QoS clamp to 0..2 ──────────────────────────────────────────
    {
        CHECK(clamp_qos(-5) == 0, "qos -5 -> 0");
        CHECK(clamp_qos(-1) == 0, "qos -1 -> 0");
        CHECK(clamp_qos(0)  == 0, "qos 0 -> 0");
        CHECK(clamp_qos(1)  == 1, "qos 1 -> 1");
        CHECK(clamp_qos(2)  == 2, "qos 2 -> 2");
        CHECK(clamp_qos(3)  == 2, "qos 3 -> 2 (clamped)");
        CHECK(clamp_qos(100) == 2, "qos 100 -> 2 (clamped)");
    }

    printf("%s (%d failure%s)\n",
           s_failures ? "FAILED" : "PASS",
           s_failures, s_failures == 1 ? "" : "s");
    return s_failures ? 1 : 0;
}
