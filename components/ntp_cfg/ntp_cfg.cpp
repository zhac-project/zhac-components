// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ntp_cfg.h"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_sntp.h"
#include "lwip/ip_addr.h"
#include "sdkconfig.h"
#include "nvs.h"
#include "zap_clock.h"

static const char* TAG = "ntp_cfg";

// Empty means "not configured": ntp_cfg_server() then reports the default.
// lwIP's sntp_setservername keeps the POINTER it is given, so this buffer (or
// the string literal default) has to outlive the client -- both are static.
static char s_server[kZapNtpHostMax] = {};
static bool s_loaded = false;

static void load_once(void) {
    if (s_loaded) return;
    s_loaded = true;
    nvs_handle_t h;
    if (nvs_open("sys_cfg", NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(s_server);
    if (nvs_get_str(h, "ntp_server", s_server, &len) != ESP_OK) s_server[0] = '\0';
    nvs_close(h);
    if (s_server[0] && !zap_ntp_host_ok(s_server)) {
        ESP_LOGW(TAG, "stored time server rejected -- using %s", kZapNtpDefault);
        s_server[0] = '\0';
    }
}

const char* ntp_cfg_server(void) {
    load_once();
    return s_server[0] ? s_server : kZapNtpDefault;
}

// A server the owner named wins outright; otherwise the router may offer one
// (DHCP option 42) and lwIP stores it at slot 0, so the default goes to
// slot 1 as the fallback. Every DHCP lease rewrites the whole list
// (dhcp_set_ntp_servers), which is why the callers re-run this on each
// address, not only the first.
#if CONFIG_LWIP_DHCP_GET_NTP_SRV && CONFIG_LWIP_SNTP_MAX_SERVERS >= 2
static constexpr bool kDhcpCapable = true;
static constexpr u8_t kDefaultSlot = 1;
#else
static constexpr bool kDhcpCapable = false;
static constexpr u8_t kDefaultSlot = 0;
#endif

static bool custom(void) { return s_server[0] != '\0'; }

void ntp_cfg_init(void) {
    load_once();
#if CONFIG_LWIP_DHCP_GET_NTP_SRV
    esp_sntp_servermode_dhcp(!custom());
    ESP_LOGI(TAG, custom() ? "time server %s (router offers ignored)"
                           : "asking the router for a time server; %s as fallback",
             ntp_cfg_server());
#endif
}

bool ntp_cfg_dhcp_server(char* out, size_t cap) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (!kDhcpCapable || custom()) return false;
    const ip_addr_t* a = esp_sntp_getserver(0);
    if (!a || ip_addr_isany(a) || esp_sntp_getservername(0) != nullptr) return false;
    return ipaddr_ntoa_r(a, out, static_cast<int>(cap)) != nullptr;
}

// Restart the client so a changed server takes effect without a reboot. Stop
// first: the running client holds the previous name pointer.
static void apply(void) {
    if (esp_sntp_enabled()) esp_sntp_stop();
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    if (custom()) {
        esp_sntp_setservername(0, s_server);
        if (kDhcpCapable) esp_sntp_setservername(1, nullptr);
    } else {
        esp_sntp_setservername(kDefaultSlot, kZapNtpDefault);
    }
    esp_sntp_init();
    char dhcp[48];
    if (ntp_cfg_dhcp_server(dhcp, sizeof(dhcp)))
        ESP_LOGI(TAG, "SNTP started: router's time server %s first, %s as fallback", dhcp, kZapNtpDefault);
    else
        ESP_LOGI(TAG, "SNTP started, time server %s", ntp_cfg_server());
}

void ntp_cfg_start(void) {
    load_once();
    apply();
}

bool ntp_cfg_set_server(const char* host) {
    char clean[kZapNtpHostMax] = {};
    if (host && host[0]) {
        if (!zap_ntp_host_ok(host)) return false;
        std::snprintf(clean, sizeof(clean), "%s", host);
    }
    nvs_handle_t h;
    if (nvs_open("sys_cfg", NVS_READWRITE, &h) != ESP_OK) return false;
    const esp_err_t e = clean[0] ? nvs_set_str(h, "ntp_server", clean)
                                 : nvs_erase_key(h, "ntp_server");
    if (e == ESP_OK) nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND) return false;   // erasing an unset key is fine
    std::memcpy(s_server, clean, sizeof(s_server));
    s_loaded = true;
#if CONFIG_LWIP_DHCP_GET_NTP_SRV
    esp_sntp_servermode_dhcp(!custom());   // takes effect at the next lease
#endif
    apply();
    return true;
}
