// WLAN station: DHCPv4 with hostname, IPv6 SLAAC, reconnect with backoff.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NETMGR_MAX_IP6 6

typedef struct {
    bool started;
    bool associated;            // WLAN link up
    bool has_ip4;
    char ssid[33];
    int8_t rssi;                // dBm, valid when associated
    uint8_t channel;
    uint8_t bssid[6];
    uint8_t mac[6];
    char hostname[33];
    esp_netif_ip_info_t ip4;
    esp_ip4_addr_t dns4[2];
    esp_ip6_addr_t ip6[NETMGR_MAX_IP6];  // global/ULA addresses, link-local filtered out
    int n_ip6;
    esp_ip6_addr_t ip6_ll;
    bool has_ip6_ll;
    uint32_t reconnects;
    uint8_t last_disconnect_reason;
    int64_t associated_since_us;
} netmgr_status_t;

esp_err_t netmgr_start(void);

// Applies changed WLAN credentials or hostname: disconnect and reconnect.
esp_err_t netmgr_restart(void);

void netmgr_get_status(netmgr_status_t *out);

esp_netif_t *netmgr_netif(void);

#ifdef __cplusplus
}
#endif
