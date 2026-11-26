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
#include "esp_timer.h"
#include "esp_wifi.h"
#include "netmgr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/ip_addr.h"

static const char *TAG = "timesync";

static SemaphoreHandle_t s_lock;   // timesync_apply() runs in the console and the event task
static bool s_inited;
static bool s_from_config;
static bool s_synced;
static time_t s_last_sync;
static uint32_t s_sync_count;
static int64_t s_sync_wall_us, s_sync_mono_us;   // at the last sync
static int64_t s_offset_us;
static bool s_offset_valid;
// lwIP keeps only the pointer to a server name (sntp_setservername() does not copy) and
// resolves the name again on every request, so the names must outlive timesync_apply().
static char s_servers[APPCFG_NTP_MAX][sizeof(((appcfg_t *)0)->ntp[0])];

static void on_sync(struct timeval *tv)
{
    // Offset: the new time against what our clock would show - the time of the previous sync
    // plus the elapsed esp_timer time. That is the drift of our clock since that sync.
    int64_t mono = esp_timer_get_time();
    int64_t wall = (int64_t)tv->tv_sec * 1000000 + tv->tv_usec;
    if (s_sync_mono_us) {
        s_offset_us = wall - (s_sync_wall_us + (mono - s_sync_mono_us));
        s_offset_valid = true;
    }
    s_sync_wall_us = wall;
    s_sync_mono_us = mono;
    s_synced = true;
    s_last_sync = tv->tv_sec;
    s_sync_count++;
    if (s_offset_valid) {
        ESP_LOGI(TAG, "event=synced utc=%lld offset_ms=%.1f syncs=%lu", (long long)tv->tv_sec, s_offset_us / 1000.0,
                 (unsigned long)s_sync_count);
    } else {
        ESP_LOGI(TAG, "event=synced utc=%lld syncs=%lu", (long long)tv->tv_sec, (unsigned long)s_sync_count);
    }
    esp_event_post(APP_EVENT, APP_EVENT_TIME_SYNCED, NULL, 0, 0);
}

void timesync_apply_tz(void)
{
    appcfg_t c;
    appcfg_get(&c);
    setenv("TZ", c.tz, 1);
    tzset();
}

esp_err_t timesync_apply(void)
{
    appcfg_t c;
    appcfg_get(&c);

    timesync_apply_tz();

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_inited) {
        esp_netif_sntp_deinit();   // SNTP stopped: nothing reads s_servers until the init below
        s_inited = false;
    }

    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(TIMESYNC_FALLBACK);
    cfg.sync_cb = on_sync;
    cfg.start = true;
    if (c.ntp[0][0]) {
        // console override: no DHCP servers
        s_from_config = true;
        cfg.server_from_dhcp = false;
        strlcpy(s_servers[0], c.ntp[0], sizeof(s_servers[0]));
        strlcpy(s_servers[1], c.ntp[1], sizeof(s_servers[1]));
        cfg.servers[0] = s_servers[0];
        cfg.num_of_servers = s_servers[1][0] ? 2 : 1;
        if (s_servers[1][0]) {
            cfg.servers[1] = s_servers[1];
        }
    } else {
        // Fallback name in slot 0. If the DHCP ACK carries option 42, lwIP puts those servers
        // in slots 0..n-1 and clears the rest (lwIP sntp.c dhcp_set_ntp_servers()).
        // Without option 42 the fallback stays. We re-apply on every disconnect, so a new
        // lease without option 42 falls back again.
        s_from_config = false;
        cfg.server_from_dhcp = true;
        cfg.num_of_servers = 1;
        cfg.servers[0] = TIMESYNC_FALLBACK;
    }
    // watch the names for resolvability (network page warns if one fails)
    const char *names[2] = {cfg.servers[0], cfg.num_of_servers > 1 ? cfg.servers[1] : NULL};
    netmgr_dns_watch("NTP", names, cfg.num_of_servers);
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err == ESP_OK) {
        s_inited = true;
    } else {
        ESP_LOGE(TAG, "event=init_failed err=%s", esp_err_to_name(err));
    }
    xSemaphoreGive(s_lock);
    return err;
}

static void on_wifi_disconnect(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    // Drop a server learned from DHCP option 42, so a new lease without option 42 falls back.
    // Nothing to do otherwise: re-initialising SNTP here would only delay the first sync.
    timesync_status_t st;
    timesync_get_status(&st);
    if (st.source == TIMESYNC_SRC_DHCP) {
        timesync_apply();
    }
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    // A request sent before the interface had an address failed, and lwIP would wait for its
    // retry timeout (15 s, doubling). Ask now that a route exists. Thread-safe (tcpip_callback).
    if (s_inited) {
        esp_sntp_restart();
    }
}

esp_err_t timesync_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_wifi_disconnect, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_GOT_IP6, on_got_ip, NULL);
    return timesync_apply();
}

void timesync_get_status(timesync_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->synced = s_synced;
    out->last_sync = s_last_sync;
    out->sync_count = s_sync_count;
    out->offset_valid = s_offset_valid;
    out->offset_us = s_offset_us;
    bool named_slot0 = false;
    bool any_server = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);   // not while timesync_apply() swaps the servers
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
    xSemaphoreGive(s_lock);
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

void timesync_iso_offset(time_t t, char *buf, size_t len)
{
    struct tm tm;
    char z[8];
    localtime_r(&t, &tm);
    strftime(z, sizeof(z), "%z", &tm);   // "+0100"
    if (strlen(z) == 5) {
        snprintf(buf, len, "%.3s:%.2s", z, z + 3);
    } else {
        strlcpy(buf, z, len);
    }
}
