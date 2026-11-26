#include "console_cmds.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "appcfg.h"
#include "board.h"
#include "carbon.h"
#include "esp_app_desc.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "health.h"
#include "i2cbus.h"
#include "metrics.h"
#include "netlog.h"
#include "netmgr.h"
#include "onewire.h"
#include "sensors.h"
#include "timesync.h"

void console_fmt_uptime(char *buf, size_t len, int64_t us, bool with_seconds)
{
    int64_t s = us / 1000000;
    int d = s / 86400;
    int h = (s / 3600) % 24;
    int m = (s / 60) % 60;
    if (with_seconds) {
        snprintf(buf, len, "%dd %02d:%02d:%02d", d, h, m, (int)(s % 60));
    } else {
        snprintf(buf, len, "%dd %02d:%02d", d, h, m);
    }
}

static const char *reset_reason(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "external";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    default: return "other";
    }
}

static void print_metric(const metric_t *m, void *arg)
{
    int64_t age_s = (esp_timer_get_time() - m->mono_us) / 1000000;
    const metric_win_t *w = &m->win[METRICS_WIN_DISPLAY];
    printf("  %-28s %12.2f   age %llds%s", m->key, m->value, (long long)age_s, metrics_is_fresh(m) ? "" : " STALE");
    if (m->state) {
        printf("   (state)\n");
    } else if (w->n) {
        // the last closed display interval: what the display shows
        printf("   interval n=%lu mean %.2f min %.2f max %.2f\n", (unsigned long)w->n, w->mean, w->min, w->max);
    } else {
        printf("\n");
    }
}

static void show_health(void)
{
    health_status_t h;
    health_get_status(&h);
    printf("health     %s, boots %lu, crashes %lu since power-on\n",
           h.safe_mode ? "SAFE MODE - sensors and display off, 'reboot' to leave" : "normal",
           (unsigned long)h.boots, (unsigned long)h.crashes);
    if (h.history_n) {
        printf("             run time before the last crashes:");
        for (int i = 0; i < h.history_n; i++) {
            printf(" %lu s", (unsigned long)h.history_s[i]);
        }
        printf(" (safe mode: %d within %d s)\n", HEALTH_SAFE_CRASHES, HEALTH_SAFE_WINDOW_S);
    }
    if (h.last_stall[0]) {
        printf("             last reset: supervisor abort, task '%s' hung\n", h.last_stall);
    }
    if (h.coredump) {
        printf("             CORE DUMP in flash - 'coredump' for the summary\n");
    }
    printf("             watched:");
    for (int i = 0; i < h.n_watch; i++) {
        if (h.watch[i].started) {
            printf(" %s %llds/%llds", h.watch[i].name, (long long)(h.watch[i].age_us / 1000000),
                   (long long)(h.watch[i].allowed_us / 1000000));
        } else {
            printf(" %s (not started)", h.watch[i].name);
        }
    }
    printf("  (last sign of life / limit)\n");
}

static void show_status(void)
{
    char buf[64];
    const esp_app_desc_t *app = esp_app_get_description();
    console_fmt_uptime(buf, sizeof(buf), esp_timer_get_time(), true);
    printf("firmware   %s %s (IDF %s)\n", app->project_name, app->version, app->idf_ver);
    printf("board      %s\n", BOARD_NAME);
    printf("uptime     %s, last reset: %s\n", buf, reset_reason());
    printf("heap       internal %u free, PSRAM %u free\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    show_health();

    netmgr_status_t n;
    netmgr_get_status(&n);
    printf("hostname   %s\n", n.hostname);
    printf("mac        " MACSTR "\n", MAC2STR(n.mac));
    if (n.associated) {
        printf("wlan       connected to '%s', RSSI %d dBm, channel %u, BSSID " MACSTR "\n", n.ssid, n.rssi,
               n.channel, MAC2STR(n.bssid));
    } else {
        printf("wlan       NOT connected (ssid '%s', last reason %u, reconnects %" PRIu32 ")\n", n.ssid,
               n.last_disconnect_reason, n.reconnects);
    }
    if (n.has_ip4) {
        printf("ipv4       " IPSTR "/" IPSTR " gw " IPSTR " dns " IPSTR " " IPSTR "\n", IP2STR(&n.ip4.ip),
               IP2STR(&n.ip4.netmask), IP2STR(&n.ip4.gw), IP2STR(&n.dns4[0]), IP2STR(&n.dns4[1]));
    } else {
        printf("ipv4       none\n");
    }
    for (int i = 0; i < n.n_ip6; i++) {
        printf("ipv6       " IPV6STR "\n", IPV62STR(n.ip6[i]));
    }
    if (n.has_ip6_ll) {
        printf("ipv6 ll    " IPV6STR "\n", IPV62STR(n.ip6_ll));
    }

    netmgr_dns_result_t dr[NETMGR_DNS_WATCH_MAX];
    int nd = netmgr_dns_results(dr, NETMGR_DNS_WATCH_MAX);
    for (int i = 0; i < nd; i++) {
        if (!dr[i].checked) {
            printf("dns        %s %s: not checked yet\n", dr[i].label, dr[i].name);
        } else if (dr[i].ok) {
            printf("dns        %s %s -> %s\n", dr[i].label, dr[i].name, dr[i].addr);
        } else {
            printf("WARNING    DNS %s %s does not resolve (getaddrinfo %d)\n", dr[i].label, dr[i].name, dr[i].err);
        }
    }

    timesync_status_t t;
    timesync_get_status(&t);
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    char off[8], zone[8];
    timesync_iso_offset(now, off, sizeof(off));
    strftime(zone, sizeof(zone), "%Z", &tm);
    printf("time       %s%s %s%s\n", buf, off, zone, t.synced ? "" : " (NOT synced)");
    printf("ntp        source %s, syncs %" PRIu32, timesync_source_name(t.source), t.sync_count);
    if (t.synced) {
        gmtime_r(&t.last_sync, &tm);
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", &tm);
        printf(", last %s", buf);
    }
    printf("\n");
    for (int i = 0; i < TIMESYNC_SLOTS; i++) {
        if (t.server[i][0]) {
            printf("  server %d  %-40s reach 0x%02x\n", i, t.server[i], t.reach[i]);
        }
    }

    for (int b = 0; b < BOARD_I2C_BUSES; b++) {
        int sda, scl;
        gpio_num_t psda, pscl;
        board_i2c_pins(b, &psda, &pscl);
        board_i2c_idle_levels(b, &sda, &scl);
        printf("i2c bus %d  SDA=GPIO%d SCL=GPIO%d, idle levels at boot SDA=%d SCL=%d\n", b + 1, psda, pscl, sda, scl);
    }
    appcfg_t c;
    appcfg_get(&c);
    uint32_t cyc = sensors_read_ms();
    printf("intervals  display %u s, carbon %u s, sensors %d s (read cycle max %lu ms)\n", c.display_s, c.carbon_s,
           sensors_interval_s(), (unsigned long)cyc);
    sensor_info_t si[SENSORS_MAX];
    int ns = sensors_list(si, SENSORS_MAX);
    for (int i = 0; i < ns; i++) {
        printf("sensor     %d:0x%02x %-8s as %-8s ok %" PRIu32 " err %" PRIu32 "  %s%s\n", si[i].bus + 1, si[i].addr, si[i].model,
               si[i].name, si[i].ok_count, si[i].err_count, si[i].supported ? "" : "NOT SUPPORTED ", si[i].info);
        if (!si[i].supported) {
            continue;
        }
        if (si[i].cadence_us) {
            printf("             own cadence %lld s", si[i].cadence_us / 1000000);
        } else {
            printf("             read %lu ms (max %lu ms) per interval", (unsigned long)si[i].read_ms,
                   (unsigned long)si[i].read_ms_max);
        }
        printf("%s%s\n", si[i].last_err ? ", failing: " : "", si[i].last_err ? esp_err_to_name(si[i].last_err) : "");
    }
    carbon_status_t cs;
    carbon_get_status(&cs);
    if (!cs.enabled) {
        printf("carbon     off\n");
    } else {
        printf("carbon     %s:%u%s, sent %lu batches / %lu lines, buffered %lu (%lu bytes), dropped %lu, errors %lu%s%s\n",
               c.carbon_host, c.carbon_port, cs.waiting_clock ? " (waiting for NTP)" : "", (unsigned long)cs.sent_batches,
               (unsigned long)cs.sent_lines, (unsigned long)cs.buffered_batches, (unsigned long)cs.buffered_bytes,
               (unsigned long)cs.dropped_batches, (unsigned long)cs.errors, cs.last_error[0] ? ", last: " : "",
               cs.last_error);
    }
    netlog_status_t ls;
    netlog_get_status(&ls);
    if (!ls.enabled) {
        printf("syslog     off (buffering %lu lines)\n", (unsigned long)ls.buffered);
    } else {
        printf("syslog     %s:%u/%s%s, sent %lu, buffered %lu, dropped %lu, errors %lu%s%s\n", c.syslog_host,
               c.syslog_port, c.syslog_tcp ? "tcp" : "udp", ls.waiting_clock ? " (waiting for NTP)" : "",
               (unsigned long)ls.sent, (unsigned long)ls.buffered, (unsigned long)ls.dropped, (unsigned long)ls.errors,
               ls.last_error[0] ? ", last: " : "", ls.last_error);
    }
    char warn[320];
    sensors_warnings(warn, sizeof(warn));
    if (warn[0]) {
        printf("WARNING    %s\n", warn);
    }
    printf("metrics\n");
    metrics_foreach(print_metric, NULL);
}

static const char *sev_name(int sev)
{
    switch (sev) {
    case APPCFG_SEV_ERROR: return "error";
    case APPCFG_SEV_WARNING: return "warning";
    case APPCFG_SEV_DEBUG: return "debug";
    default: return "info";
    }
}

static void show_config(void)
{
    appcfg_t c;
    appcfg_get(&c);
    printf("hostname   %s\n", c.hostname);
    printf("wifi       ssid '%s', psk %s (%s)\n", c.wifi_ssid, c.wifi_psk[0] ? "********" : "(none)",
           appcfg_wifi_from_build() ? "from wlan.config" : "NVS");
    printf("tz         %s\n", c.tz);
    if (c.ntp[0][0]) {
        printf("ntp        %s %s\n", c.ntp[0], c.ntp[1]);
    } else {
        printf("ntp        dhcp (fallback %s)\n", TIMESYNC_FALLBACK);
    }
    printf("interval   display %u s, carbon %u s\n", c.display_s, c.carbon_s);
    if (c.carbon_host[0]) {
        printf("carbon     %s port %u prefix %s\n", c.carbon_host, c.carbon_port, c.carbon_prefix);
    } else {
        printf("carbon     off (prefix %s)\n", c.carbon_prefix);
    }
    if (c.syslog_host[0]) {
        printf("syslog     %s port %u %s, level %s\n", c.syslog_host, c.syslog_port, c.syslog_tcp ? "tcp" : "udp",
               sev_name(c.syslog_level));
    } else {
        printf("syslog     off\n");
    }
    printf("prefer     %s (carbon, syslog)\n", c.prefer_v4 ? "IPv4" : "IPv6");
    printf("%s\n", appcfg_dirty() ? "UNSAVED changes - 'save' to keep them" : "saved");
}

// Next DST transitions as this device computes them (checks the TZ rule, not the clock).
static void show_tz(void)
{
    appcfg_t c;
    appcfg_get(&c);
    printf("TZ rule    %s\n", c.tz);
    time_t t = time(NULL);
    if (t < 1700000000) {
        t = 1794787200;   // 2026-11-16 00:00 UTC when the clock is not set yet
    }
    t -= t % 3600;
    struct tm prev, cur;
    localtime_r(&t, &prev);
    int found = 0;
    for (int h = 1; h <= 24 * 800 && found < 4; h++) {
        time_t u = t + (time_t)h * 3600;
        localtime_r(&u, &cur);
        if (cur.tm_isdst != prev.tm_isdst) {
            time_t before = u - 1;
            struct tm tb, tu;
            char a[40], b[40], ob[8], ou[8], ut[24];
            localtime_r(&before, &tb);
            gmtime_r(&u, &tu);
            strftime(ut, sizeof(ut), "%Y-%m-%dT%H:%M:%SZ", &tu);
            strftime(a, sizeof(a), "%Y-%m-%dT%H:%M:%S", &tb);
            strftime(b, sizeof(b), "%Y-%m-%dT%H:%M:%S", &cur);
            timesync_iso_offset(before, ob, sizeof(ob));
            timesync_iso_offset(u, ou, sizeof(ou));
            char zb[8], zc[8];
            strftime(zb, sizeof(zb), "%Z", &tb);
            strftime(zc, sizeof(zc), "%Z", &cur);
            printf("switch at  %s: %s%s %s -> %s%s %s\n", ut, a, ob, zb, b, ou, zc);
            found++;
        }
        prev = cur;
    }
    if (!found) {
        printf("no DST switch in the next 800 days\n");
    }
}

static int cmd_show(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "status") == 0) {
        show_status();
    } else if (strcmp(argv[1], "config") == 0) {
        show_config();
    } else if (strcmp(argv[1], "tz") == 0) {
        show_tz();
    } else {
        printf("usage: show status|config|tz\n");
        return 1;
    }
    return 0;
}

static int report(esp_err_t err, const char *what)
{
    if (err != ESP_OK) {
        printf("%s: %s\n", what, esp_err_to_name(err));
        return 1;
    }
    printf("ok - 'save' to keep it\n");
    return 0;
}

// "set interval": checks and hints. Errors refuse the value, hints only explain.
static void interval_help(void)
{
    appcfg_t c;
    appcfg_get(&c);
    static char help[640];
    sensors_cadence_help(help, sizeof(help));
    printf("usage: set interval display|carbon <%d..%d s>   (now display %u s, carbon %u s)\n\n"
           "display  the panel refreshes at every boundary of this interval (60 s = at the start of\n"
           "         every minute, aligned to UTC once NTP synced) and shows the mean of the readings\n"
           "         in the interval.\n"
           "carbon   at every boundary one batch goes to Carbon: mean, and min/max where the interval\n"
           "         held more than one reading.\n"
           "sensors  sensors without a cadence of their own are read once per sensor interval = the\n"
           "         shorter of the two, so that the read ends 100 ms before the boundary. Sensors\n"
           "         with a cadence keep it - their algorithm or calibration depends on it:\n%s"
           "Best: one interval a multiple of the other (60/60, 30/60, 60/300).\n",
           APPCFG_INTERVAL_MIN, APPCFG_INTERVAL_MAX, c.display_s, c.carbon_s, help);
}

static int set_interval(int argc, char **argv)
{
    if (argc != 4 || (strcmp(argv[2], "display") != 0 && strcmp(argv[2], "carbon") != 0)) {
        interval_help();
        return argc == 2 ? 0 : 1;   // plain "set interval" asks for the explanation
    }
    bool disp = strcmp(argv[2], "display") == 0;
    int v = atoi(argv[3]);
    if (v < APPCFG_INTERVAL_MIN || v > APPCFG_INTERVAL_MAX) {
        printf("error: %d..%d s\n", APPCFG_INTERVAL_MIN, APPCFG_INTERVAL_MAX);
        return 1;
    }
    // the read cycle of the sensors without a cadence plus its lead must fit into an interval
    uint32_t need_ms = sensors_read_ms() + 100;
    if ((uint32_t)v * 1000 < need_ms) {
        printf("error: reading the sensors takes up to %lu ms (incl. 100 ms lead), %d s is shorter\n",
               (unsigned long)need_ms, v);
        return 1;
    }
    if (report(appcfg_set_intervals(disp ? v : 0, disp ? 0 : v), "set interval")) {
        return 1;
    }
    appcfg_t c;
    appcfg_get(&c);
    int lo = c.display_s < c.carbon_s ? c.display_s : c.carbon_s;
    int hi = c.display_s < c.carbon_s ? c.carbon_s : c.display_s;
    printf("display %u s, carbon %u s: sensors without a cadence are read every %d s\n", c.display_s, c.carbon_s, lo);
    if (hi % lo != 0) {
        printf("note: %d s is not a multiple of %d s. That works, but is not recommended: the boundaries\n"
               "      drift against each other, so a %s interval holds sometimes %d, sometimes %d readings,\n"
               "      and %s lags up to %d s behind the reading.\n",
               hi, lo, c.display_s > c.carbon_s ? "display" : "carbon", hi / lo, hi / lo + 1,
               c.display_s > c.carbon_s ? "the display" : "Carbon", lo - 1);
    }
    if (c.display_s % 60 != 0 && 60 % c.display_s != 0) {
        printf("note: the clock shows minutes; with %u s it changes up to %u s after the minute turns\n", c.display_s,
               c.display_s - 1);
    }
    sensors_reschedule();
    carbon_reschedule();
    return 0;
}

static int cmd_set(int argc, char **argv)
{
    if (argc >= 4 && strcmp(argv[1], "wifi") == 0) {
        if (report(appcfg_set_wifi(argv[2], argv[3]), "set wifi (ssid 1-32, psk 8-64 chars)")) {
            return 1;
        }
        netmgr_restart();
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "hostname") == 0) {
        if (report(appcfg_set_hostname(argv[2]), "set hostname ([a-z0-9-], max 32)")) {
            return 1;
        }
        printf("reconnecting so DHCP sends the new name\n");
        netmgr_restart();
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "tz") == 0) {
        if (report(appcfg_set_tz(argv[2]), "set tz")) {
            return 1;
        }
        timesync_apply_tz();   // display only; SNTP and its DHCP servers stay as they are
        return 0;
    }
    if (argc == 4 && strcmp(argv[1], "co2") == 0 && strcmp(argv[2], "outdoor") == 0) {
        esp_err_t err = sensors_scd4x_set_outdoor(atoi(argv[3]));
        printf("%s\n", err == ESP_OK ? "saved (ASC target follows after reboot)" : "outdoor CO2: 380..500 ppm");
        return err == ESP_OK ? 0 : 1;
    }
    if (argc >= 3 && strcmp(argv[1], "ntp") == 0) {
        esp_err_t err = strcmp(argv[2], "dhcp") == 0 ? appcfg_set_ntp(NULL, NULL)
                                                      : appcfg_set_ntp(argv[2], argc >= 4 ? argv[3] : NULL);
        if (report(err, "set ntp")) {
            return 1;
        }
        timesync_apply();
        if (strcmp(argv[2], "dhcp") == 0) {
            printf("until the next DHCP ACK (lease renewal or reconnect) the fallback %s is used\n",
                   TIMESYNC_FALLBACK);
        }
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "interval") == 0) {
        return set_interval(argc, argv);
    }
    if (argc >= 3 && strcmp(argv[1], "carbon") == 0) {
        esp_err_t err;
        if (strcmp(argv[2], "off") == 0) {
            err = appcfg_set_carbon(NULL, 0, NULL);
        } else {
            err = appcfg_set_carbon(argv[2], argc >= 4 ? atoi(argv[3]) : 0, argc >= 5 ? argv[4] : NULL);
        }
        if (report(err, "set carbon <host> [port] [prefix] (prefix: [A-Za-z0-9_-] nodes with dots)")) {
            return 1;
        }
        carbon_reschedule();
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "syslog") == 0) {
        esp_err_t err;
        if (strcmp(argv[2], "level") == 0 && argc == 4) {
            int sev = strcmp(argv[3], "error") == 0     ? APPCFG_SEV_ERROR
                      : strcmp(argv[3], "warning") == 0 ? APPCFG_SEV_WARNING
                      : strcmp(argv[3], "info") == 0    ? APPCFG_SEV_INFO
                      : strcmp(argv[3], "debug") == 0   ? APPCFG_SEV_DEBUG
                                                        : -1;
            err = appcfg_set_syslog_level(sev);
        } else if (strcmp(argv[2], "off") == 0) {
            err = appcfg_set_syslog(NULL, 0, false);
        } else {
            bool tcp = argc >= 5 && strcmp(argv[4], "tcp") == 0;
            if (argc >= 5 && !tcp && strcmp(argv[4], "udp") != 0) {
                err = ESP_ERR_INVALID_ARG;
            } else {
                err = appcfg_set_syslog(argv[2], argc >= 4 ? atoi(argv[3]) : 0, tcp);
            }
        }
        if (report(err, "set syslog <host> [port] [udp|tcp] | off | level error|warning|info|debug")) {
            return 1;
        }
        netlog_apply();
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "prefer") == 0 && (strcmp(argv[2], "v4") == 0 || strcmp(argv[2], "v6") == 0)) {
        appcfg_set_prefer_v4(strcmp(argv[2], "v4") == 0);
        return 0;
    }
    printf("usage:\n"
           "  set wifi <ssid> <psk>          WPA2/WPA3 personal\n"
           "  set hostname <name>            DHCP option 12\n"
           "  set tz <posix>                 e.g. UTC0 or CET-1CEST,M3.5.0,M10.5.0/3\n"
           "  set ntp <server> [<server>]    overrides DHCP option 42\n"
           "  set ntp dhcp                   back to DHCP option 42 / fallback\n"
           "  set co2 outdoor <ppm>          fresh-air target for the SCD41 (default 430)\n"
           "  set interval display|carbon <s>   %d..%d s, default %d ('set interval' explains)\n"
           "  set carbon <host> [port] [prefix] Graphite plaintext, TCP, default port %d\n"
           "  set carbon off\n"
           "  set syslog <host> [port] [udp|tcp]  RFC 5424, default port %d udp\n"
           "  set syslog off | level error|warning|info|debug\n"
           "  set prefer v6|v4               address family tried first (carbon, syslog)\n",
           APPCFG_INTERVAL_MIN, APPCFG_INTERVAL_MAX, APPCFG_INTERVAL_DEFAULT, APPCFG_CARBON_PORT, APPCFG_SYSLOG_PORT);
    return 1;
}

static int cmd_save(int argc, char **argv)
{
    esp_err_t err = appcfg_save();
    printf("%s\n", err == ESP_OK ? "saved" : esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_reboot(int argc, char **argv)
{
    if (appcfg_dirty()) {
        printf("note: unsaved changes are lost\n");
    }
    sensors_save_state();
    printf("rebooting\n");
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    return 0;
}

static int cmd_factory_reset(int argc, char **argv)
{
    if (argc != 2 || strcmp(argv[1], "yes") != 0) {
        printf("erases all settings and the BSEC calibration state. Type: factory-reset yes\n");
        return 1;
    }
    esp_err_t err = appcfg_factory_reset();
    printf("NVS erased: %s, rebooting\n", esp_err_to_name(err));
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    return 0;
}

static int cmd_i2c(int argc, char **argv)
{
    if (argc != 2 || strcmp(argv[1], "scan") != 0) {
        printf("usage: i2c scan\n");
        return 1;
    }
    for (int b = 0; b < i2cbus_count(); b++) {
        uint8_t found[128];
        sensors_bus_lock();
        int n = i2cbus_scan(b, found);
        sensors_bus_unlock();
        printf("bus %d: %d device(s):", b + 1, n);
        for (int a = 0; a < 128; a++) {
            if (found[a]) {
                printf(" 0x%02x", a);
            }
        }
        printf("\n");
    }
    return 0;
}

static void ow_print(void)
{
    sensors_ow_dev_t d[OW_MAX_DEVICES];
    int found = 0;
    int n = sensors_onewire_list(d, OW_MAX_DEVICES, &found);
    if (n < 0) {
        printf("no DS2484 found\n");
        return;
    }
    if (found > OW_MAX_DEVICES) {
        printf("ERROR: %d%s 1-Wire devices found, max %d - using the first %d\n", found,
               found >= OW_SEARCH_LIMIT ? "+" : "", OW_MAX_DEVICES, OW_MAX_DEVICES);
    }
    printf("%d device(s) in use\n", n);
    for (int i = 0; i < n; i++) {
        char rom[OW_ROM_STR_LEN];
        ow_rom_str(d[i].rom, rom);
        printf("  %d %s %-19s ", i + 1, rom, d[i].name);
        if (ow_family(d[i].rom) != OW_FAMILY_DS18B20) {
            printf("family 0x%02x, not read\n", ow_family(d[i].rom));
        } else if (d[i].valid) {
            printf("%7.2f C  crc errors %" PRIu32 "\n", d[i].temp_c, d[i].crc_errors);
        } else {
            printf("     --  crc errors %" PRIu32 "\n", d[i].crc_errors);
        }
    }
}

static int cmd_onewire(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "scan") == 0) {
        uint32_t before = sensors_onewire_rescan();
        for (int i = 0; i < 50 && sensors_onewire_scan_count() == before; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        ow_print();
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "list") == 0) {
        ow_print();
        return 0;
    }
    if (argc >= 3 && (strcmp(argv[1], "name") == 0 || strcmp(argv[1], "forget") == 0)) {
        uint64_t rom;
        if (!ow_rom_parse(argv[2], &rom)) {
            printf("ROM ID: 16 hex digits as shown by 'onewire scan'\n");
            return 1;
        }
        bool forget = strcmp(argv[1], "forget") == 0;
        if (!forget && argc != 4) {
            printf("usage: onewire name <rom> <name>\n");
            return 1;
        }
        esp_err_t err = ow_name_set(rom, forget ? NULL : argv[3]);
        if (err == ESP_ERR_INVALID_ARG) {
            printf("name: 1-%d chars [a-z0-9_], not starting with ow_\n", OW_NAME_LEN - 1);
        } else if (err == ESP_ERR_INVALID_STATE) {
            printf("name already used by another device\n");
        } else if (err == ESP_ERR_NO_MEM) {
            printf("name table full (%d) - 'onewire forget' an old one\n", OW_NAMES_MAX);
        } else if (err != ESP_OK) {
            printf("%s\n", esp_err_to_name(err));
        } else {
            printf("saved\n");
            sensors_onewire_rescan();   // picks up the new name
        }
        return err == ESP_OK ? 0 : 1;
    }
    if (argc >= 2 && strcmp(argv[1], "names") == 0) {
        uint64_t roms[OW_NAMES_MAX];
        char names[OW_NAMES_MAX][OW_NAME_LEN];
        int n = ow_names_list(roms, names, OW_NAMES_MAX);
        printf("%d of %d names stored\n", n, OW_NAMES_MAX);
        for (int i = 0; i < n; i++) {
            char rom[OW_ROM_STR_LEN];
            ow_rom_str(roms[i], rom);
            printf("  %s %s\n", rom, names[i]);
        }
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "selftest") == 0) {
        int n = argc >= 3 ? atoi(argv[2]) : OW_MAX_DEVICES + 1;
        char *rep = malloc(1024);
        if (!rep) {
            return 1;
        }
        bool ok = ow_selftest(n, rep, 1024);
        printf("%s", rep);
        free(rep);
        return ok ? 0 : 1;
    }
    printf("usage:\n"
           "  onewire scan                 search the bus, list devices\n"
           "  onewire list                 last search result\n"
           "  onewire name <rom> <name>    name a probe (saved at once)\n"
           "  onewire forget <rom>         remove a name\n"
           "  onewire names                stored names, also of absent probes\n"
           "  onewire selftest [n]         search on a simulated bus with n devices\n");
    return 1;
}

static int cmd_sensor_show(int argc, char **argv);

static int cmd_scd41(int argc, char **argv)
{
    char *msg = malloc(SCD4X_MSG_LEN);
    if (!msg) {
        return 1;
    }
    const size_t len = SCD4X_MSG_LEN;
    esp_err_t err;
    const char *sub = argc >= 2 ? argv[1] : "";
    if (argc == 3 && strcmp(sub, "offset") == 0 && strcmp(argv[2], "auto") == 0) {
        err = sensors_scd4x_offset_auto(msg, len);
    } else if (argc == 3 && strcmp(sub, "offset") == 0 && strcmp(argv[2], "reset") == 0) {
        err = sensors_scd4x_offset_reset(msg, len);
    } else if (argc == 3 && strcmp(sub, "offset") == 0) {
        err = sensors_scd4x_offset_set(strtof(argv[2], NULL), msg, len);
    } else if ((argc == 3 || argc == 4) && strcmp(sub, "frc") == 0) {
        int ppm = atoi(argv[2]);
        if (ppm < 300 || ppm > 2000) {
            printf("target 300..2000 ppm\n");
            free(msg);
            return 1;
        }
        err = sensors_scd4x_frc(ppm, argc == 4 && strcmp(argv[3], "force") == 0, msg, len);
    } else if (argc == 2 && strcmp(sub, "diag") == 0) {
        printf("stopping measurement, self test takes 10 s ...\n");
        err = sensors_scd4x_diag(msg, len);
    } else if (argc == 2 && (strcmp(sub, "status") == 0)) {
        free(msg);
        char *a[] = {"sensor", "scd41"};
        return cmd_sensor_show(2, a);
    } else if (argc == 2 && strcmp(sub, "health") == 0) {
        sensors_scd4x_health_print();
        free(msg);
        return 0;
    } else if (argc == 3 && strcmp(sub, "health") == 0) {
        err = sensors_scd4x_health_interval(atoi(argv[2]));
        strlcpy(msg, err == ESP_OK ? "saved" : "minutes 0..1440", len);
    } else if (argc == 2 && strcmp(sub, "single") == 0) {
        printf("single shot takes 5 s ...\n");
        err = sensors_scd4x_single_shot(msg, len);
    } else if (argc == 2 && strcmp(sub, "reinit") == 0) {
        err = sensors_scd4x_reinit(false, msg, len);
    } else if (argc == 2 && strcmp(sub, "sleep-cycle") == 0) {
        err = sensors_scd4x_reinit(true, msg, len);
    } else if (argc == 3 && strcmp(sub, "asc") == 0 && (strcmp(argv[2], "on") == 0 || strcmp(argv[2], "off") == 0)) {
        err = sensors_scd4x_asc(strcmp(argv[2], "on") == 0, msg, len);
    } else if (argc == 3 && strcmp(sub, "log") == 0 && (strcmp(argv[2], "on") == 0 || strcmp(argv[2], "off") == 0)) {
        sensors_scd4x_log_raw(strcmp(argv[2], "on") == 0);
        free(msg);
        return 0;
    } else if (strcmp(sub, "factory-reset") == 0) {
        if (argc != 3 || strcmp(argv[2], "yes") != 0) {
            printf("Erases ALL calibration of the sensor (every FRC, the ASC history) and its EEPROM settings,\n"
                   "back to the state from the factory. Cannot be undone. Type: scd41 factory-reset yes\n");
            free(msg);
            return 1;
        }
        err = sensors_scd4x_factory_reset(msg, len);
    } else if (argc == 2 && strcmp(sub, "fresh-air") == 0) {
        err = sensors_scd4x_fresh_air_start();
        strlcpy(msg, err == ESP_OK ? "started, progress on the display" : esp_err_to_name(err), len);
    } else {
        printf("usage:\n"
               "  scd41 status              detail page (offset, calibration, fresh air)\n"
               "  scd41 diag                serial, variant, ASC, offset, altitude, pressure, last raw\n"
               "                            reading, 10 s self test (changes nothing)\n"
               "  scd41 health [<min>]      log / min. interval of checks on implausible CO2 (default 30, 0 = off)\n"
               "  scd41 single              one single-shot measurement (5 s), outside periodic mode\n"
               "  scd41 log on|off          print every raw reading (also with 'log off')\n"
               "  scd41 reinit              reload the sensor settings from its EEPROM\n"
               "  scd41 sleep-cycle         power_down + wake_up (software power cycle)\n"
               "  scd41 asc on|off          automatic self-calibration (RAM only)\n"
               "  scd41 offset auto         temperature offset against the SHT4x (needs 2 min of readings)\n"
               "  scd41 offset <C>          set the temperature offset\n"
               "  scd41 offset reset        forget it, use the sensor's EEPROM value\n"
               "  scd41 frc <ppm> [force]   forced recalibration; refused unless CO2 was stable for 3 min\n"
               "                            and within 150 ppm of <ppm> ([force] skips the checks)\n"
               "  scd41 fresh-air           same as a long press on BOOT: FRC to 'set co2 outdoor' once stable\n"
               "  scd41 factory-reset yes   erase all calibration (FRC, ASC history) - irreversible\n");
        free(msg);
        return 1;
    }
    printf("%s\n", msg);
    free(msg);
    return err == ESP_OK ? 0 : 1;
}

static int cmd_sensor_show(int argc, char **argv)
{
    sensor_info_t si[SENSORS_MAX];
    int n = sensors_list(si, SENSORS_MAX);
    for (int i = 0; i < n; i++) {
        if (argc == 2 && strcmp(argv[1], si[i].name) != 0) {
            continue;
        }
        printf("%s (%s at bus %d 0x%02x): %s\n", si[i].name, si[i].model, si[i].bus + 1, si[i].addr, si[i].info);
        char *buf = malloc(1024);
        if (buf && sensors_detail(i, buf, 1024)) {
            for (char *p = buf; *p; p++) {
                if (*p == '\f') {
                    *p = '\n';   // columns on the display, one after the other here
                }
            }
            printf("%s\n", buf);
        }
        free(buf);
    }
    return 0;
}

static int cmd_wifi(int argc, char **argv)
{
    if (argc == 2 && (strcmp(argv[1], "on") == 0 || strcmp(argv[1], "off") == 0)) {
        esp_err_t err = netmgr_radio(strcmp(argv[1], "on") == 0);
        printf("WLAN radio %s: %s\n", argv[1], esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    printf("usage: wifi on|off   (debug: radio off, e.g. to test supply interference)\n");
    return 1;
}

static int cmd_led(int argc, char **argv)
{
    if (argc == 2 && (strcmp(argv[1], "on") == 0 || strcmp(argv[1], "off") == 0)) {
        board_led_set(strcmp(argv[1], "on") == 0);
        return 0;
    }
    printf("usage: led on|off   (red LED D1 on GPIO41, off after boot)\n");
    return 1;
}

static int cmd_coredump(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "erase") == 0) {
        return health_coredump_erase() == ESP_OK ? (printf("erased\n"), 0) : 1;
    }
    if (argc != 1) {
        printf("usage: coredump [erase]\n");
        return 1;
    }
    char line[200];
    esp_err_t err = health_coredump_summary(line, sizeof(line));
    if (err == ESP_ERR_NOT_FOUND) {
        printf("no core dump in flash\n");
        return 0;
    }
    if (err != ESP_OK) {
        printf("core dump unreadable: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("%s\n", line);
    printf("full analysis over USB: idf.py coredump-info (or coredump-debug for gdb), then 'coredump erase'\n");
    return 0;
}

static int cmd_crash(int argc, char **argv)
{
    static const char *usage =
        "crash panic|fault|spin|irqoff|stall - TEST: crash or hang the hub on purpose\n"
        "  panic   esp_system_abort()                   panic handler, core dump\n"
        "  fault   store to address 0                   exception, core dump\n"
        "  spin    endless loop pinned to core 1        task watchdog (IDLE1 starves), core dump\n"
        "  irqoff  endless loop, interrupts off         interrupt watchdog, core dump\n"
        "  stall   hold the sensor bus lock forever     sensors task hangs, supervisor aborts, core dump\n";
    if (argc != 2) {
        printf("%s", usage);
        return 1;
    }
    if (strcmp(argv[1], "stall") == 0) {
        sensors_bus_lock();   // never released: the sensors task blocks on its next bus access
        printf("sensor bus locked; the supervisor aborts once the sensors task is overdue (see 'show status')\n");
        return 0;
    }
    if (strcmp(argv[1], "panic") && strcmp(argv[1], "fault") && strcmp(argv[1], "spin") &&
        strcmp(argv[1], "irqoff")) {
        printf("%s", usage);
        return 1;
    }
    health_crash(argv[1]);
    return 0;
}

static int cmd_version(int argc, char **argv)
{
    const esp_app_desc_t *app = esp_app_get_description();
    printf("%s %s, built %s %s, IDF %s\n", app->project_name, app->version, app->date, app->time, app->idf_ver);
    return 0;
}

static int cmd_log(int argc, char **argv)
{
    // console only; what goes to syslog is set with "set syslog level"
    if (argc == 2 && strcmp(argv[1], "off") == 0) {
        netlog_set_console_level(ESP_LOG_WARN);
    } else if (argc == 2 && strcmp(argv[1], "on") == 0) {
        netlog_set_console_level(ESP_LOG_INFO);
    } else {
        printf("usage: log on|off   (off = warnings and errors only)\n");
        return 1;
    }
    return 0;
}

esp_err_t console_cmds_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t rc = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    rc.prompt = "hub> ";
    rc.task_stack_size = 6144;
    esp_console_dev_uart_config_t uc = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uc, &rc, &repl));

    const esp_console_cmd_t cmds[] = {
        {.command = "show", .help = "show status | show config", .func = cmd_show},
        {.command = "set", .help = "set wifi|hostname|tz|ntp ... (no args: usage)", .func = cmd_set},
        {.command = "save", .help = "write settings to NVS", .func = cmd_save},
        {.command = "reboot", .help = "restart (saves BSEC state)", .func = cmd_reboot},
        {.command = "factory-reset", .help = "erase all settings: factory-reset yes", .func = cmd_factory_reset},
        {.command = "i2c", .help = "i2c scan", .func = cmd_i2c},
        {.command = "onewire", .help = "onewire scan|list|name|forget|names|selftest", .func = cmd_onewire},
        {.command = "led", .help = "led on|off", .func = cmd_led},
        {.command = "wifi", .help = "wifi on|off (debug)", .func = cmd_wifi},
        {.command = "sensor", .help = "sensor [name]: driver details (as on the display page)", .func = cmd_sensor_show},
        {.command = "scd41", .help = "scd41 status|diag|single|log|reinit|sleep-cycle|asc|offset|frc|fresh-air|factory-reset", .func = cmd_scd41},
        {.command = "version", .help = "firmware version", .func = cmd_version},
        {.command = "coredump", .help = "coredump [erase]: core dump summary / erase after fetching", .func = cmd_coredump},
        {.command = "crash", .help = "crash panic|fault|spin|irqoff|stall (TEST)", .func = cmd_crash},
        {.command = "log", .help = "log on|off", .func = cmd_log},
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }
    esp_console_register_help_command();
    return esp_console_start_repl(repl);
}
