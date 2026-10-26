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

// 32 bytes from 64 hex digits, either case, optionally with a colon between bytes; spaces around the
// whole are ignored. False for anything else.
inline bool zap_tls_pin_parse(const char* s, uint8_t out[32]) {
    if (!s) return false;
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
