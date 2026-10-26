// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// zap_tls -- how the hub's TLS clients judge a server: the `crt_bundle_attach` callbacks of
// esp_http_client, esp-tls, esp-mqtt and the websocket client. The pure rules are in zap_tls_policy.h.
//
//   zap_tls_attach         IDF's CA bundle (esp_crt_bundle_attach), and the certificate dates once the
//                          clock is set (review 2026-10-24 HS-11; the build sets MBEDTLS_HAVE_TIME_DATE).
//   zap_tls_attach_broker  the MQTT broker: as zap_tls_attach, or, with a pin set, only the certificate
//                          whose SHA-256 is the pin, whoever signed it and whatever name it carries (HS-13:
//                          a home broker with its own CA or a self-signed certificate).
#pragma once

#include <cstdint>

#include "esp_err.h"

esp_err_t zap_tls_attach(void* conf);

// The pin zap_tls_attach_broker checks the next connection against; nullptr for none. Set it before the
// client (re)connects.
void      zap_tls_set_broker_pin(const uint8_t* sha256);
esp_err_t zap_tls_attach_broker(void* conf);
