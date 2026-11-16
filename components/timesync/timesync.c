#include "timesync.h"

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "app_events.h"
#include "appcfg.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "lwip/ip_addr.h"

static const char *TAG = "timesync";

static bool s_inited;
static bool s_from_config;
static bool s_synced;
static time_t s_last_sync;
static uint32_t s_sync_count;

static void on_sync(struct timeval *tv)
{
    s_synced = true;
    s_last_sync = tv->tv_sec;
    s_sync_count++;
    ESP_LOGI(TAG, "time synced: %lld", (long long)tv->tv_sec);
    esp_event_post(APP_EVENT, APP_EVENT_TIME_SYNCED, NULL, 0, 0);
}

esp_err_t timesync_apply(void)
{
    appcfg_t c;
    appcfg_get(&c);

    setenv("TZ", c.tz, 1);
    tzset();

    if (s_inited) {
        esp_netif_sntp_deinit();
        s_inited = false;
    }

    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(TIMESYNC_FALLBACK_V4);
    cfg.sync_cb = on_sync;
    cfg.start = true;
    if (c.ntp[0][0]) {
        // console override: no DHCP servers
        s_from_config = true;
        cfg.server_from_dhcp = false;
        cfg.servers[0] = c.ntp[0];
        cfg.num_of_servers = c.ntp[1][0] ? 2 : 1;
        if (c.ntp[1][0]) {
            cfg.servers[1] = c.ntp[1];
        }
    } else {
        // Fallback servers in slots 0 and 1. If the DHCP ACK carries option 42, lwIP puts those
        // servers in slots 0..n-1 and clears the rest (lwIP sntp.c dhcp_set_ntp_servers()).
        // Without option 42 the fallback stays. We re-apply on every disconnect, so a new
        // lease without option 42 falls back again.
        s_from_config = false;
        cfg.server_from_dhcp = true;
        cfg.num_of_servers = 2;
        cfg.servers[0] = TIMESYNC_FALLBACK_V4;
        cfg.servers[1] = TIMESYNC_FALLBACK_V6;
    }
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err == ESP_OK) {
        s_inited = true;
    } else {
        ESP_LOGE(TAG, "sntp init: %s", esp_err_to_name(err));
    }
    return err;
}

static void on_wifi_disconnect(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (!s_from_config) {
        timesync_apply();
    }
}

esp_err_t timesync_start(void)
{
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_wifi_disconnect, NULL);
    return timesync_apply();
}

void timesync_get_status(timesync_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->synced = s_synced;
    out->last_sync = s_last_sync;
    out->sync_count = s_sync_count;
    bool named_slot0 = false;
    bool any_server = false;
    if (s_inited) {
        for (int i = 0; i < TIMESYNC_SLOTS; i++) {
            const char *name = esp_sntp_getservername(i);
            const ip_addr_t *ip = esp_sntp_getserver(i);
            if (name && name[0]) {
                strlcpy(out->server[i], name, sizeof(out->server[i]));
                if (i == 0) {
                    named_slot0 = true;
                }
            } else if (ip && !ip_addr_isany(ip)) {
                ipaddr_ntoa_r(ip, out->server[i], sizeof(out->server[i]));
            }
            if (out->server[i][0]) {
                any_server = true;
            }
            out->reach[i] = esp_sntp_getreachability(i);
        }
    }
    if (s_from_config) {
        out->source = TIMESYNC_SRC_CONFIG;
    } else if (any_server && !named_slot0) {
        // slot 0 set by address, not by name: written by the DHCP client
        out->source = TIMESYNC_SRC_DHCP;
    } else {
        out->source = TIMESYNC_SRC_FALLBACK;
    }
}

const char *timesync_source_name(timesync_source_t s)
{
    switch (s) {
    case TIMESYNC_SRC_DHCP:
        return "DHCP";
    case TIMESYNC_SRC_CONFIG:
        return "config";
    default:
        return "fallback";
    }
}
