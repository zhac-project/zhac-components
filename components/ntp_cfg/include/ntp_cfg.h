// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// ntp_cfg -- the hub's time server: which one, and starting SNTP with it.
//
// No ZHAC board has a battery-backed clock, so every boot starts at 1970 and
// schedules wait (see zap_clock.h and simple_rules' cron task). A hub with
// internet access reaches the public default; a hub on a network without it
// can be pointed at a local server instead, which is the only way its clock
// comes back after a power cut without someone opening the web UI.
#pragma once

#include <stddef.h>

// Start SNTP with the configured server. Call once the interface has an
// address; safe to call again (it restarts the client with the same server).
// Right after the network stack exists (esp_netif_init has run, so after
// eth_start / wifi_start) and before the first DHCP lease lands: lets lwIP
// hand the router's time server (DHCP option 42) to SNTP unless the owner
// named one. It runs in the TCP/IP thread; called earlier it asserts.
void ntp_cfg_init(void);
// On every address (each DHCP lease): (re)starts the client with the
// configured server -- next to the router's offer, which lwIP put at slot 0.
void ntp_cfg_start(void);
// The router-offered server currently in use, as text; false when none.
bool ntp_cfg_dhcp_server(char* out, size_t cap);

// Persist a new server and restart SNTP with it, no reboot. An empty or null
// host clears the setting, which restores the public default. Returns false
// when the host is not acceptable (zap_ntp_host_ok) or NVS refuses it.
bool ntp_cfg_set_server(const char* host);

// The server in use: the configured one, else the public default. Stable
// pointer, safe to hand to the status encoders.
const char* ntp_cfg_server(void);
