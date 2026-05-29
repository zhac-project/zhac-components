// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef void (*mqtt_rx_cb_t)(const char* topic,   int topic_len,
                              const char* payload, int payload_len);

void mqtt_gw_init();
/* `payload_len` is the exact number of bytes from `payload` to publish.
 * Must be passed explicitly — callers MUST NOT rely on strlen() of an
 * arbitrary buffer, as HAP payloads are not NUL-terminated and reading
 * past the end walks uninitialised memory (and stalls the outbox when
 * the spurious length is huge). Pass strlen(payload) yourself for
 * NUL-terminated strings. */
void mqtt_gw_publish(const char* topic, const char* payload, size_t payload_len,
                      int qos, bool retain);
/* Reconfigure broker URL and restart client; no-op on P4 */
void mqtt_gw_set_broker_url(const char* url);

/* Start the MQTT client using the currently-configured broker URL and
 * client id. No-op when the client is already running. Clears the
 * auto-disable fail counter. Use after flipping the `mqtt_enabled`
 * settings flag at runtime so the change takes effect without a reboot.
 * S3 only — no-op on P4. */
void mqtt_gw_start();

/* Stop and destroy the MQTT client. Publishes become no-ops until
 * `mqtt_gw_start()` (or broker-url change) runs again. S3 only. */
void mqtt_gw_stop();

/* Store broker URL / root / client-id without starting the client.
 * Boot path uses this so esp-mqtt doesn't spin up before STA is up
 * (connect-then-disconnect loops would otherwise trip the auto-
 * disable counter in a few seconds). Empty / null args leave the
 * corresponding field untouched. S3 only. */
void mqtt_gw_configure(const char* url, const char* root, const char* cid);

/* Call from the WiFi STA-got-IP handler. If MQTT is enabled and
 * configured but the client isn't running, start it. Idempotent —
 * safe to call on every reconnect. S3 only. */
void mqtt_gw_on_sta_up();

/* S3 only — no-ops on P4 */
bool mqtt_gw_is_connected();
bool mqtt_gw_is_active();  // true if client exists (not auto-disabled)
/* True only when the configured broker URL is a verified-TLS scheme
 * (mqtts:// / wss://). Used to gate cleartext-sensitive publishes (e.g. the
 * metrics snapshot, F45). Always false on P4 (no local client). */
bool mqtt_gw_is_secure();
void mqtt_gw_set_rx_callback(mqtt_rx_cb_t cb);
void mqtt_gw_subscribe(const char* topic_filter, int qos);

/* MQTT client identity. Must be unique per broker. Default on boot is
 * `zhac-<last-4-hex-of-mac>`. Setting a non-null, non-empty string
 * triggers a client restart (same cost as set_broker_url). NVS-persisted
 * under mqtt_cfg::client_id. S3 only. */
void mqtt_gw_set_client_id(const char* id);

/* Root topic prefix used for every ZHAC-originated MQTT publish. Default
 * `zhac`. Letting the operator override this lets two controllers
 * coexist on one broker (e.g. `home/zhac-kitchen` vs `home/zhac-garage`).
 * NVS-persisted under mqtt_cfg::root_topic. S3 only. */
void mqtt_gw_set_root_topic(const char* root);

/* Build a topic by joining the configured root and `suffix` with '/'.
 * Example: mqtt_gw_format_topic(buf, sizeof(buf), "log/info") produces
 * "<root>/log/info". Returns bytes written (excluding NUL) or -1 on
 * overflow. Suffix may start with '/' or not. S3 only. */
int  mqtt_gw_format_topic(char* out, size_t cap, const char* suffix);

/* Returns the configured root topic (without trailing slash). Useful
 * for subscribers and prefix checks on inbound topics. Always returns
 * a NUL-terminated non-empty string (defaults to `"zhac"`). */
const char* mqtt_gw_get_root_topic(void);
