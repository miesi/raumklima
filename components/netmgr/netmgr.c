#include "netmgr.h"

#include <string.h>

#include "app_events.h"
#include "appcfg.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/ip6_addr.h"
#include "sdkconfig.h"

static const char *TAG = "netmgr";

static esp_netif_t *s_netif;
static SemaphoreHandle_t s_lock;
static netmgr_status_t s_st;
static esp_timer_handle_t s_retry_timer;
static uint32_t s_backoff_ms = 1000;
static bool s_stopping;

#define BACKOFF_MAX_MS 60000

static void notify(void)
{
    esp_event_post(APP_EVENT, APP_EVENT_NET_CHANGED, NULL, 0, 0);
}

static void retry_cb(void *arg)
{
    ESP_LOGI(TAG, "reconnecting");
    esp_wifi_connect();
}

static void apply_config(void)
{
    appcfg_t c;
    appcfg_get(&c);

    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, c.wifi_ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, c.wifi_psk, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;   // WPA2-PSK or better (WPA2/WPA3 mixed, WPA3-SAE)
    wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    wc.sta.pmf_cfg.capable = true;
    wc.sta.pmf_cfg.required = false;
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;       // with several APs (mesh) pick the strongest
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));

    // option 12 is sent by the DHCP client, which starts on association
    ESP_ERROR_CHECK(esp_netif_set_hostname(s_netif, c.hostname));

    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_st.ssid, c.wifi_ssid, sizeof(s_st.ssid));
    strlcpy(s_st.hostname, c.hostname, sizeof(s_st.hostname));
    xSemaphoreGive(s_lock);
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case WIFI_EVENT_STA_START: {
        appcfg_t c;
        appcfg_get(&c);
        if (c.wifi_ssid[0] == '\0') {
            ESP_LOGW(TAG, "no WLAN configured - use 'set wifi <ssid> <psk>' on the console");
            return;
        }
        esp_wifi_connect();
        break;
    }
    case WIFI_EVENT_STA_CONNECTED: {
        const wifi_event_sta_connected_t *e = data;
        ESP_LOGI(TAG, "associated with '%.*s' channel %d", e->ssid_len, (const char *)e->ssid, e->channel);
        // SLAAC needs a link-local address first
        esp_netif_create_ip6_linklocal(s_netif);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_st.associated = true;
        s_st.channel = e->channel;
        memcpy(s_st.bssid, e->bssid, 6);
        s_st.associated_since_us = esp_timer_get_time();
        xSemaphoreGive(s_lock);
        s_backoff_ms = 1000;
        notify();
        break;
    }
    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *e = data;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool was = s_st.associated;
        s_st.associated = false;
        s_st.has_ip4 = false;
        s_st.n_ip6 = 0;
        s_st.has_ip6_ll = false;
        s_st.last_disconnect_reason = e->reason;
        if (was) {
            s_st.reconnects++;
        }
        xSemaphoreGive(s_lock);
        if (s_stopping) {
            return;
        }
        ESP_LOGW(TAG, "disconnected, reason %d, retry in %lu ms", e->reason, (unsigned long)s_backoff_ms);
        esp_timer_stop(s_retry_timer);
        esp_timer_start_once(s_retry_timer, (uint64_t)s_backoff_ms * 1000);
        s_backoff_ms = s_backoff_ms * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : s_backoff_ms * 2;
        notify();
        break;
    }
    default:
        break;
    }
}

static void refresh_ip6(void)
{
    esp_ip6_addr_t all[CONFIG_LWIP_IPV6_NUM_ADDRESSES];
    int n = esp_netif_get_all_ip6(s_netif, all);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.n_ip6 = 0;
    s_st.has_ip6_ll = false;
    for (int i = 0; i < n; i++) {
        if (esp_netif_ip6_get_addr_type(&all[i]) == ESP_IP6_ADDR_IS_LINK_LOCAL) {
            s_st.ip6_ll = all[i];
            s_st.has_ip6_ll = true;
        } else if (s_st.n_ip6 < NETMGR_MAX_IP6) {
            s_st.ip6[s_st.n_ip6++] = all[i];
        }
    }
    xSemaphoreGive(s_lock);
}

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        esp_netif_dns_info_t d0 = {0}, d1 = {0};
        esp_netif_get_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &d0);
        esp_netif_get_dns_info(s_netif, ESP_NETIF_DNS_BACKUP, &d1);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_st.has_ip4 = true;
        s_st.ip4 = e->ip_info;
        s_st.dns4[0] = d0.ip.u_addr.ip4;
        s_st.dns4[1] = d1.ip.u_addr.ip4;
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "IPv4 " IPSTR "/" IPSTR " gw " IPSTR, IP2STR(&e->ip_info.ip), IP2STR(&e->ip_info.netmask),
                 IP2STR(&e->ip_info.gw));
        notify();
    } else if (id == IP_EVENT_GOT_IP6) {
        const ip_event_got_ip6_t *e = data;
        if (e->esp_netif != s_netif) {
            return;
        }
        ESP_LOGI(TAG, "IPv6 " IPV6STR, IPV62STR(e->ip6_info.ip));
        refresh_ip6();
        notify();
    } else if (id == IP_EVENT_STA_LOST_IP) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_st.has_ip4 = false;
        xSemaphoreGive(s_lock);
        notify();
    }
}

esp_err_t netmgr_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(esp_netif_init());
    s_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));   // credentials live in appcfg, not in the WLAN driver's NVS
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, on_ip, NULL));

    const esp_timer_create_args_t ta = {.callback = retry_cb, .name = "wifi_retry"};
    ESP_ERROR_CHECK(esp_timer_create(&ta, &s_retry_timer));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    esp_wifi_get_mac(WIFI_IF_STA, s_st.mac);
    apply_config();
    s_st.started = true;
    return esp_wifi_start();
}

esp_err_t netmgr_restart(void)
{
    s_stopping = true;
    esp_timer_stop(s_retry_timer);
    esp_wifi_disconnect();
    apply_config();
    s_stopping = false;
    s_backoff_ms = 1000;
    return esp_wifi_connect();
}

void netmgr_get_status(netmgr_status_t *out)
{
    wifi_ap_record_t ap;
    bool have_ap = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (have_ap) {
        s_st.rssi = ap.rssi;
    }
    *out = s_st;
    xSemaphoreGive(s_lock);
}

esp_netif_t *netmgr_netif(void)
{
    return s_netif;
}

void netmgr_ip6_str(const esp_ip6_addr_t *a, char *buf, size_t len)
{
    ip6_addr_t ip;
    memcpy(ip.addr, a->addr, sizeof(ip.addr));
#if LWIP_IPV6_SCOPES
    ip.zone = 0;
#endif
    ip6addr_ntoa_r(&ip, buf, (int)len);
}
