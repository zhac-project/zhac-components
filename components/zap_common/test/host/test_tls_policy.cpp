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
    char line[160];
    std::snprintf(line, sizeof(line), "sha256 Fingerprint=%s\n", colons);
    CHECK(zap_tls_pin_parse(line, again) && std::memcmp(pin, again, 32) == 0,
          "openssl's whole line, \"sha256 Fingerprint=\" and all");

    // Certificate times as UTC seconds, and the firmware's build day as a floor (LOW review M2).
    CHECK(zap_utc_seconds(1970, 1, 1, 0, 0, 0) == 0 && zap_utc_seconds(2026, 10, 26, 0, 0, 0) == 1792972800 &&
          zap_utc_seconds(2000, 3, 1, 12, 30, 15) == 951913815 && zap_utc_seconds(2024, 2, 29, 23, 59, 59) == 1709251199,
          "civil date-times to UTC seconds, leap day included");
    CHECK(zap_build_day("Oct 26 2026") == 1792972800 && zap_build_day("Oct  6 2026") == 1791244800,
          "the build day as __DATE__ writes it, a padded day too");
    CHECK(zap_build_day("") == 0 && zap_build_day(nullptr) == 0 && zap_build_day("Foo 26 2026") == 0 &&
          zap_build_day("Oct 26 20") == 0, "a date that does not parse gives no floor");
    constexpr uint32_t kExpired = 0x01;
    const int64_t built = 1792972800;   // 2026-10-26
    CHECK(zap_tls_cert_flags(0, kDates, kExpired, 0, built - 86400, built) == kExpired,
          "clock not set: a certificate that ended before the build day has still expired");
    CHECK(zap_tls_cert_flags(0, kDates, kExpired, kZapClockSetAfter + 1000, built - 86400, built) == kExpired,
          "a clock set back into its validity does not bring it back");
    CHECK(zap_tls_cert_flags(kDates, kDates, kExpired, 0, built + 86400, built) == 0,
          "clock not set, ends after the build: the dates are left out as before");
    CHECK(zap_tls_cert_flags(kOther, kDates, kExpired, 0, built - 86400, 0) == kOther,
          "no build day known: no floor");
    CHECK(zap_tls_cert_flags(0, kDates, kExpired, 0, zap_utc_seconds(0, 1, 1, 0, 0, 0), built) == 0,
          "a time before 1970 (IDF's synthetic bundle roots carry year 0) is not judged");

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
