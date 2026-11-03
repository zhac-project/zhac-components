// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: Apache-2.0
//
// zap_tls_policy.h -- the pure parts of how the hub judges a TLS server (zap_tls is the IDF glue).
//
// Certificate dates (review 2026-10-24 HS-11): mbedTLS checks them once built with
// MBEDTLS_HAVE_TIME_DATE, against time(). No ZHAC board keeps the time over a power cut, so until SNTP or
// the web UI sets the clock it reads 1970 and every certificate would look not yet valid: the dates are
// left out then, and count as soon as the clock is set.
//
// Broker pin (HS-13): an MQTT broker with its own CA or a self-signed certificate is trusted by the
// SHA-256 of its certificate, as `openssl x509 -noout -fingerprint -sha256` prints it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>

#include "zap_clock.h"

// The verify flags of one certificate with `date_bits` (BADCERT_EXPIRED | BADCERT_FUTURE) left out
// while the clock at `now` is not set.
inline uint32_t zap_tls_date_flags(uint32_t flags, uint32_t date_bits, time_t now) {
    return zap_clock_is_set(now) ? flags : (flags & ~date_bits);
}

// UTC seconds of a civil date and time (proleptic Gregorian; days_from_civil), for certificate times.
inline int64_t zap_utc_seconds(int y, int m, int d, int hh, int mm, int ss) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (era * 146097 + doe - 719468) * 86400 + hh * 3600 + mm * 60 + ss;
}

// The start (UTC) of the day this firmware was built, from esp_app_desc_t::date as __DATE__ writes it
// ("Oct 26 2026", "Oct  6 2026"); 0 when it does not parse.
inline int64_t zap_build_day(const char* date) {
    static const char kMon[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    if (!date || std::strlen(date) != 11 || date[3] != ' ' || date[6] != ' ') return 0;
    int m = 0;
    while (m < 12 && std::strncmp(kMon + 3 * m, date, 3) != 0) m++;
    const auto digit = [](char c) { return c >= '0' && c <= '9'; };
    if (m == 12 || !digit(date[5]) || !(date[4] == ' ' || digit(date[4]))) return 0;
    const int d = (date[4] == ' ' ? 0 : date[4] - '0') * 10 + (date[5] - '0');
    int y = 0;
    for (int i = 7; i < 11; i++) {
        if (!digit(date[i])) return 0;
        y = y * 10 + (date[i] - '0');
    }
    return d >= 1 && d <= 31 ? zap_utc_seconds(y, m + 1, d, 0, 0, 0) : 0;
}

// What the hub makes of one certificate's date flags (`date_bits` = BADCERT_EXPIRED | BADCERT_FUTURE):
// left out while the clock is not set, as zap_tls_date_flags; and, whatever the clock says, a certificate
// that ended (`not_after`, UTC seconds) before the day this firmware was built has expired, so a clock set
// back by whoever answers NTP cannot bring it back (LOW review M2). No floor when the build day is unknown;
// a time before 1970 (IDF's synthetic bundle roots carry year 0) is not judged.
inline uint32_t zap_tls_cert_flags(uint32_t flags, uint32_t date_bits, uint32_t expired_bit, time_t now,
                                   int64_t not_after, int64_t built_day) {
    flags = zap_tls_date_flags(flags, date_bits, now);
    if (built_day > 0 && not_after > 0 && not_after < built_day) flags |= expired_bit;
    return flags;
}

// 32 bytes from 64 hex digits, either case, optionally with a colon between bytes; spaces around the
// whole are ignored, and so is a label up to an '=' (openssl prints "sha256 Fingerprint=AB:CD:..").
// False for anything else.
inline bool zap_tls_pin_parse(const char* s, uint8_t out[32]) {
    if (!s) return false;
    if (const char* eq = std::strrchr(s, '=')) s = eq + 1;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (int i = 0; i < 32; i++) {
        if (i > 0 && *s == ':') s++;
        const int hi = nib(s[0]);
        const int lo = hi < 0 ? -1 : nib(s[1]);
        if (lo < 0) return false;
        out[i] = static_cast<uint8_t>(hi << 4 | lo);
        s += 2;
    }
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    return *s == '\0';
}

// A URL that sends a user name or password without TLS: mqtt:// or ws:// with user[:password]@ before
// the host. Settings warns about it (HS-13).
inline bool zap_url_cleartext_credentials(const char* url) {
    if (!url) return false;
    const char* host = nullptr;
    if (std::strncmp(url, "mqtt://", 7) == 0)    host = url + 7;
    else if (std::strncmp(url, "ws://", 5) == 0) host = url + 5;
    else return false;
    for (const char* p = host; *p && *p != '/' && *p != '?' && *p != '#'; ++p)
        if (*p == '@') return true;
    return false;
}

// The stored form: 64 lower-case hex digits and a NUL.
inline void zap_tls_pin_format(const uint8_t pin[32], char out[65]) {
    static const char kHex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[2 * i]     = kHex[pin[i] >> 4];
        out[2 * i + 1] = kHex[pin[i] & 0xF];
    }
    out[64] = '\0';
}
