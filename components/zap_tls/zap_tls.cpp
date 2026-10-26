// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// zap_tls.cpp -- see zap_tls.h. mbedTLS calls the verify callback once per certificate of the chain,
// root first, with that certificate's flags, and ORs what the callback leaves into the verdict
// (x509_crt_merge_flags_with_cb); the name check's flag sits on the server's own certificate, depth 0.
#include "zap_tls.h"

#include <atomic>
#include <cstring>
#include <ctime>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"
#include "zap_tls_policy.h"

static const char* TAG = "zap_tls";

// IDF's check of a chain against its bundle (esp_crt_bundle.c): what esp_crt_bundle_attach installs. Not
// in its header, but a public symbol; calling it keeps that check exactly as it is.
extern "C" int esp_crt_verify_callback(void* buf, mbedtls_x509_crt* crt, int depth, uint32_t* flags);

namespace {

constexpr uint32_t kDateBits = MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE;

// The start of the day this firmware was built: no certificate that ended before it is valid, whatever
// the clock says (zap_tls_cert_flags). 0 when the build carries no date.
int64_t built_day() {
    static const int64_t day = [] {
        const esp_app_desc_t* app = esp_app_get_description();
        return app ? zap_build_day(app->date) : int64_t{0};
    }();
    return day;
}

// The dates go first: the bundle's callback looks up the trust anchor only for a link whose sole fault
// is "not trusted", and before the clock is set every certificate also reads "not yet valid".
int verify(void* ctx, mbedtls_x509_crt* crt, int depth, uint32_t* flags) {
    const mbedtls_x509_time& t = crt->valid_to;
    const int64_t not_after = zap_utc_seconds(t.year, t.mon, t.day, t.hour, t.min, t.sec);
    *flags = zap_tls_cert_flags(*flags, kDateBits, MBEDTLS_X509_BADCERT_EXPIRED, time(nullptr), not_after,
                                built_day());
    return esp_crt_verify_callback(ctx, crt, depth, flags);
}

uint8_t           s_pin[32];
std::atomic<bool> s_pin_set{false};

// Pinned: the server's own certificate must be the pinned one; what signed it and the name in it do not
// matter, so the rest of the chain is not judged.
int verify_broker(void* ctx, mbedtls_x509_crt* crt, int depth, uint32_t* flags) {
    if (!s_pin_set.load()) return verify(ctx, crt, depth, flags);
    if (depth > 0) { *flags = 0; return 0; }
    uint8_t h[32];
    size_t n = 0;
    const bool same = psa_hash_compute(PSA_ALG_SHA_256, crt->raw.p, crt->raw.len, h, sizeof(h), &n) == PSA_SUCCESS &&
                      n == sizeof(h) && std::memcmp(h, s_pin, sizeof(h)) == 0;
    if (same) {
        *flags = 0;
    } else {
        *flags |= MBEDTLS_X509_BADCERT_NOT_TRUSTED;
        ESP_LOGW(TAG, "the broker's certificate is not the pinned one");
    }
    return 0;
}

esp_err_t attach(void* conf, int (*cb)(void*, mbedtls_x509_crt*, int, uint32_t*)) {
    const esp_err_t err = esp_crt_bundle_attach(conf);
    if (err == ESP_OK && conf) mbedtls_ssl_conf_verify(static_cast<mbedtls_ssl_config*>(conf), cb, nullptr);
    return err;
}

}  // namespace

esp_err_t zap_tls_attach(void* conf) { return attach(conf, verify); }

void zap_tls_set_broker_pin(const uint8_t* sha256) {
    if (sha256) std::memcpy(s_pin, sha256, sizeof(s_pin));
    s_pin_set.store(sha256 != nullptr);
}

esp_err_t zap_tls_attach_broker(void* conf) { return attach(conf, verify_broker); }
