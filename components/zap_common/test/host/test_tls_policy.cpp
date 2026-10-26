// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: Apache-2.0
//
// zap_tls_policy.h: certificate dates count once the clock is set (review 2026-10-24 HS-11), and the
// broker certificate pin an owner types in for mqtts:// (HS-13).
#include "zap_tls_policy.h"

#include <cstdio>
#include <cstring>

static int g_fail = 0;
#define CHECK(c, m) do { if (c) std::printf("PASS: %s\n", m); else { std::printf("FAIL: %s\n", m); g_fail++; } } while (0)

int main() {
    constexpr uint32_t kDates = 0x01 | 0x0200;   // mbedTLS BADCERT_EXPIRED | BADCERT_FUTURE
    constexpr uint32_t kOther = 0x08;            // BADCERT_NOT_TRUSTED
    CHECK(zap_tls_date_flags(kDates | kOther, kDates, 0) == kOther,
          "clock not set (1970): the dates cannot be judged and are dropped, other faults stay");
    CHECK(zap_tls_date_flags(kDates | kOther, kDates, kZapClockSetAfter) == (kDates | kOther),
          "clock set: an expired or not-yet-valid certificate fails");
    CHECK(zap_tls_date_flags(0, kDates, 0) == 0, "nothing to drop");

    uint8_t pin[32];
    const char* colons = "AB:CD:EF:01:23:45:67:89:ab:cd:ef:01:23:45:67:89:AB:CD:EF:01:23:45:67:89:ab:cd:ef:01:23:45:67:89";
    CHECK(zap_tls_pin_parse(colons, pin) && pin[0] == 0xAB && pin[1] == 0xCD && pin[31] == 0x89,
          "a fingerprint as openssl prints it (colons, either case)");
    char hex[65];
    zap_tls_pin_format(pin, hex);
    CHECK(std::strcmp(hex, "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789") == 0,
          "stored as 64 lower-case hex digits");
    uint8_t again[32];
    CHECK(zap_tls_pin_parse(hex, again) && std::memcmp(pin, again, 32) == 0, "bare hex reads back the same");
    CHECK(zap_tls_pin_parse("  abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789\n", again),
          "spaces around it are dropped");
    CHECK(!zap_tls_pin_parse("abcdef0123456789abcdef0123456789abcdef0123456789abcdef012345678", again),
          "63 digits: refused");
    CHECK(!zap_tls_pin_parse("abcdef0123456789abcdef0123456789abcdef0123456789abcdef01234567890", again),
          "65 digits: refused");
    CHECK(!zap_tls_pin_parse("gbcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789", again),
          "a letter that is not hex: refused");
    CHECK(!zap_tls_pin_parse("ab:cd::ef", again) && !zap_tls_pin_parse("", again) && !zap_tls_pin_parse(nullptr, again),
          "empty, null and malformed: refused");
    CHECK(!zap_tls_pin_parse("a:bcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789", again),
          "a colon inside a byte: refused");

    // A broker URL that sends its user name and password in clear (HS-13: Settings warns).
    CHECK(zap_url_cleartext_credentials("mqtt://user:pass@192.168.1.10:1883"), "mqtt:// with user:pass@");
    CHECK(zap_url_cleartext_credentials("ws://user@broker/mqtt"), "ws:// with a user name");
    CHECK(!zap_url_cleartext_credentials("mqtts://user:pass@broker:8883"), "mqtts:// is encrypted");
    CHECK(!zap_url_cleartext_credentials("wss://user:pass@broker/mqtt"), "wss:// is encrypted");
    CHECK(!zap_url_cleartext_credentials("mqtt://broker:1883"), "no credentials");
    CHECK(!zap_url_cleartext_credentials("ws://broker/path@x"), "an @ in the path is not a user name");
    CHECK(!zap_url_cleartext_credentials("") && !zap_url_cleartext_credentials(nullptr), "empty and null");
    std::printf("%s\n", g_fail ? "FAILED" : "ok");
    return g_fail;
}
