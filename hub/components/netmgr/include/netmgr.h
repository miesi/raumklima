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

// Debug: radio off/on (e.g. to see whether WLAN current peaks disturb a sensor).
esp_err_t netmgr_radio(bool on);

// --- DNS watch: configured host names (NTP, later the metrics target) are resolved every
// 5 min and after each new address; failures are shown on the network page.
#define NETMGR_DNS_WATCH_MAX 4
typedef struct {
    char label[8];      // e.g. "NTP"
    char name[64];
    bool checked;
    bool ok;
    char addr[48];      // first address returned
    int err;            // getaddrinfo() error when !ok
    int64_t when_us;
} netmgr_dns_result_t;

// Replaces the names watched under this label. IP literals are skipped.
void netmgr_dns_watch(const char *label, const char *const *names, int n);
int netmgr_dns_results(netmgr_dns_result_t *out, int max);

esp_netif_t *netmgr_netif(void);

// RFC 5952 text form (zero compression), e.g. "2003:e5:6f22:3000::1".
void netmgr_ip6_str(const esp_ip6_addr_t *a, char *buf, size_t len);

#ifdef __cplusplus
}
#endif
