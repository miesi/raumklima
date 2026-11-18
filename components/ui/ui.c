// E-paper UI.
//
// Rendering: LVGL renders the whole 792 x 272 frame into a 1 bpp buffer
// (LV_DISPLAY_RENDER_MODE_FULL, LV_COLOR_FORMAT_I1). The frame goes to the panel in one go
// (~27 kB per RAM, a few ms of SPI); the SSD1683 compares it with the previous frame and
// moves only the pixels that changed (partial refresh). Every UI_FULL_EVERY partial
// refreshes, or on OK, a full refresh clears ghosting.
//
// Update policy: once per minute at the minute boundary, immediately on a key press, and
// shortly after network or time events. Only this task touches LVGL.
#include "ui.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "app_events.h"
#include "appcfg.h"
#include "board.h"
#include "console_cmds.h"
#include "epd.h"
#include "esp_app_desc.h"
#include "esp_console.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "fonts.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "metrics.h"
#include "netmgr.h"
#include "sched.h"
#include "sensors.h"
#include "timesync.h"

static const char *TAG = "ui";

#define UI_FULL_EVERY     30      // partial refreshes between two full refreshes
#define UI_EVENT_SETTLE_MS 1500   // coalesce bursts of network events

typedef enum { MSG_KEY, MSG_REDRAW } msg_type_t;
typedef struct {
    msg_type_t type;
    app_key_event_t key;
} ui_msg_t;

static QueueHandle_t s_q;
static lv_display_t *s_disp;
static uint8_t *s_frame;          // last frame rendered by LVGL, without palette
static uint8_t *s_shown;          // last frame sent to the panel
static int s_stride;
static bool s_frame_new;
static int s_partials_since_full = UI_FULL_EVERY;   // first frame is a full refresh
static bool s_force_full;
static int s_page;

#define CO2_BACK_STREAK 12          // plausible SCD4x readings (1 min) before the tile trusts it again
static bool s_co2_from_scd = true;
static uint32_t s_co2_faults_seen;

// --- page model ---

// order: overview, network, time, one page per sensor, system (last)
enum { PAGE_OVERVIEW, PAGE_NETWORK, PAGE_TIME, PAGE_SYSTEM, PAGE_FIXED_COUNT };

static int page_count(void)
{
    sensor_info_t si[SENSORS_MAX];
    return PAGE_FIXED_COUNT + sensors_list(si, SENSORS_MAX);
}

// --- LVGL objects ---

static lv_obj_t *s_overview, *s_detail;
static lv_obj_t *s_clock, *s_date, *s_ntp;
static lv_obj_t *s_tile_cap[6], *s_tile_val[6];
static lv_obj_t *s_uptime, *s_warn, *s_pageno;
static lv_obj_t *s_ow_cap[OW_MAX_DEVICES], *s_ow_val[OW_MAX_DEVICES];
static lv_obj_t *s_overlay, *s_ovl_text;
static lv_obj_t *s_title, *s_col[2], *s_pageno2;

static lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font, int x, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_black(), 0);
    lv_obj_set_pos(l, x, y);
    lv_label_set_text(l, "");
    return l;
}

static void set_text(lv_obj_t *l, const char *txt)
{
    // avoid invalidating (and refreshing the panel) when nothing changed
    if (strcmp(lv_label_get_text(l), txt) != 0) {
        lv_label_set_text(l, txt);
    }
}

static lv_obj_t *mk_container(lv_obj_t *scr)
{
    lv_obj_t *c = lv_obj_create(scr);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, EPD_WIDTH, EPD_HEIGHT);
    lv_obj_set_pos(c, 0, 0);
    return c;
}

static void build_screen(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // overview
    s_overview = mk_container(scr);
    s_clock = mk_label(s_overview, &font_dejavu_bold_72, 10, 0);
    s_date = mk_label(s_overview, &font_dejavu_bold_24, 14, 86);
    s_ntp = mk_label(s_overview, &font_dejavu_14, 14, 120);
    for (int i = 0; i < 6; i++) {
        int x = 330 + (i % 2) * 235;
        int y = 2 + (i / 2) * 52;
        s_tile_cap[i] = mk_label(s_overview, &font_dejavu_14, x, y);
        s_tile_val[i] = mk_label(s_overview, &font_dejavu_bold_32, x, y + 14);
    }
    lv_obj_t *rule = lv_obj_create(s_overview);
    lv_obj_remove_style_all(rule);
    lv_obj_set_size(rule, EPD_WIDTH - 20, 2);
    lv_obj_set_pos(rule, 10, 160);
    lv_obj_set_style_bg_color(rule, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    s_uptime = mk_label(s_overview, &font_dejavu_14, 14, 140);
    s_warn = mk_label(s_overview, &font_dejavu_14, 14, 165);
    lv_obj_set_width(s_warn, 715);
    lv_label_set_long_mode(s_warn, LV_LABEL_LONG_MODE_CLIP);
    s_pageno = mk_label(s_overview, &font_dejavu_14, 740, 165);
    for (int i = 0; i < OW_MAX_DEVICES; i++) {
        int x = 14 + (i % 4) * 195;
        int y = 186 + (i / 4) * 43;
        s_ow_cap[i] = mk_label(s_overview, &font_dejavu_14, x, y);
        s_ow_val[i] = mk_label(s_overview, &font_dejavu_bold_24, x, y + 14);
    }

    // detail pages: title + two text columns
    s_detail = mk_container(scr);
    s_title = mk_label(s_detail, &font_dejavu_bold_24, 14, 4);
    s_col[0] = mk_label(s_detail, &font_dejavu_18, 14, 40);
    s_col[1] = mk_label(s_detail, &font_dejavu_18, 410, 40);
    s_pageno2 = mk_label(s_detail, &font_dejavu_14, 740, 254);
    lv_obj_set_hidden(s_detail, true);

    // network-down overlay, above both pages
    s_overlay = lv_obj_create(scr);
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, 560, 96);
    lv_obj_set_pos(s_overlay, (EPD_WIDTH - 560) / 2, (EPD_HEIGHT - 96) / 2);
    lv_obj_set_style_bg_color(s_overlay, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_overlay, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_overlay, 4, 0);
    lv_obj_t *t = mk_label(s_overlay, &font_dejavu_bold_24, 16, 8);
    lv_label_set_text(t, "NETWORK DOWN");
    s_ovl_text = mk_label(s_overlay, &font_dejavu_18, 16, 40);
    lv_obj_set_hidden(s_overlay, true);
}

// --- formatting helpers ---

// value of a sensor that measures the room air (not a by-product such as the SCD4x temperature)
static void fmt_metric(char *buf, size_t len, const char *quantity, const char *fmt)
{
    metric_t m;
    if (metrics_get_ambient(quantity, &m)) {
        snprintf(buf, len, fmt, metrics_display_value(&m));   // mean over the display interval
    } else {
        strlcpy(buf, "--", len);
    }
}

static void fmt_time(char *buf, size_t len, const char *fmt, time_t t)
{
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, len, fmt, &tm);
}

#define APPEND(buf, ...) snprintf((buf) + strlen(buf), sizeof(buf) - strlen(buf), __VA_ARGS__)

// --- pages ---

static void page_overview(int page, int count)
{
    char b[96];
    timesync_status_t t;
    timesync_get_status(&t);
    time_t now = time(NULL);

    if (t.synced) {
        fmt_time(b, sizeof(b), "%H:%M", now);
        set_text(s_clock, b);
        // no ISO offset here: it runs into the pressure tile (time page shows it)
        fmt_time(b, sizeof(b), "%a %Y-%m-%d  %Z", now);
        set_text(s_date, b);
        char last[16];
        fmt_time(last, sizeof(last), "%H:%M", t.last_sync);
        snprintf(b, sizeof(b), "NTP %s (%s), synced %s", t.server[0][0] ? t.server[0] : "-",
                 timesync_source_name(t.source), last);
    } else {
        set_text(s_clock, "--:--");
        set_text(s_date, "time not set");
        snprintf(b, sizeof(b), "NTP not synced yet (%s)", timesync_source_name(t.source));
    }
    set_text(s_ntp, b);

    // tiles: value from the best sensor for the quantity, with fallbacks
    metric_t m;
    // 0 temperature, 1 humidity, 2 pressure
    set_text(s_tile_cap[0], "Temperature");
    fmt_metric(b, sizeof(b), "temperature_c", "%.1f \xc2\xb0" "C");
    set_text(s_tile_val[0], b);
    set_text(s_tile_cap[1], "Humidity");
    fmt_metric(b, sizeof(b), "humidity_pct", "%.0f %%");
    set_text(s_tile_val[1], b);
    set_text(s_tile_cap[2], "Pressure");
    fmt_metric(b, sizeof(b), "pressure_hpa", "%.0f hPa");
    set_text(s_tile_val[2], b);
    // 3 air quality: BSEC IAQ, else raw gas resistance
    if (metrics_get_primary("iaq", &m)) {
        metric_t acc;
        bool has_acc = metrics_get_primary("iaq_accuracy", &acc);
        snprintf(b, sizeof(b), "IAQ (accuracy %.0f of 3)", has_acc ? acc.value : 0.0f);
        set_text(s_tile_cap[3], b);
        snprintf(b, sizeof(b), "%.0f", metrics_display_value(&m));
    } else if (metrics_get_primary("gas_ohm", &m)) {
        set_text(s_tile_cap[3], "Gas resistance (no IAQ)");
        snprintf(b, sizeof(b), "%.0f kOhm", metrics_display_value(&m) / 1000.0f);
    } else {
        set_text(s_tile_cap[3], "IAQ");
        strlcpy(b, "--", sizeof(b));
    }
    set_text(s_tile_val[3], b);
    // 4 CO2: measured (SCD4x); the BSEC estimate only if there is no SCD4x at all. With an SCD4x
    // the tile shows "--" from the first implausible reading and the value again only after
    // CO2_BACK_STREAK plausible readings in a row, so the caption does not flip every few seconds.
    scd4x_co2_state_t cs;
    bool have_scd = sensors_scd4x_co2_state(&cs);
    if (have_scd) {
        // any fault since the last update counts, also one the minute snapshot would miss
        if (cs.faults != s_co2_faults_seen) {
            s_co2_faults_seen = cs.faults;
            s_co2_from_scd = false;
        } else if (!s_co2_from_scd && cs.good_streak >= CO2_BACK_STREAK) {
            s_co2_from_scd = true;
        }
    }
    if (have_scd && s_co2_from_scd) {
        int days;
        if (sensors_scd4x_calibration_age(&days)) {
            snprintf(b, sizeof(b), "CO2 (calibrated %d d ago)", days);
        } else {
            strlcpy(b, "CO2 (not calibrated)", sizeof(b));
        }
        set_text(s_tile_cap[4], b);
        if (metrics_get_primary("co2_ppm", &m)) {
            snprintf(b, sizeof(b), "%.0f ppm", metrics_display_value(&m));
        } else {
            strlcpy(b, "--", sizeof(b));
        }
    } else if (have_scd) {
        // SCD41 present but faulty: "--", not the BSEC estimate (2200 ppm seen indoors)
        set_text(s_tile_cap[4], "CO2 (SCD41 fault)");
        strlcpy(b, "--", sizeof(b));
    } else if (metrics_get_primary("co2eq_ppm", &m)) {
        // no SCD41 at all: the BSEC estimate, marked as such (Thomas, 2026-11-17)
        set_text(s_tile_cap[4], "CO2 eq. (BSEC est.)");
        snprintf(b, sizeof(b), "%.0f ppm", metrics_display_value(&m));
    } else {
        set_text(s_tile_cap[4], "CO2");
        strlcpy(b, "--", sizeof(b));
    }
    set_text(s_tile_val[4], b);
    set_text(s_tile_cap[5], "Light");
    fmt_metric(b, sizeof(b), "illuminance_lx", "%.0f lx");
    set_text(s_tile_val[5], b);

    char up[24];
    console_fmt_uptime(up, sizeof(up), esp_timer_get_time(), false);
    snprintf(b, sizeof(b), "Uptime %s", up);
    set_text(s_uptime, b);

    // alert line: sensor limits, fresh-air hint, calibration progress
    char w[160];
    sensors_warnings(w, sizeof(w));
    if (w[0]) {
        char a[sizeof(w) + 4];
        snprintf(a, sizeof(a), "! %s", w);
        set_text(s_warn, a);
    } else {
        set_text(s_warn, "");
    }

    // 1-Wire probes: up to OW_MAX_DEVICES tiles, 4 per row
    sensors_ow_dev_t ow[OW_MAX_DEVICES];
    int found = 0;
    int n_ow = sensors_onewire_list(ow, OW_MAX_DEVICES, &found);
    for (int i = 0; i < OW_MAX_DEVICES; i++) {
        if (i < n_ow) {
            set_text(s_ow_cap[i], ow[i].name);
            if (ow_family(ow[i].rom) != OW_FAMILY_DS18B20) {
                strlcpy(b, "?", sizeof(b));
            } else if (ow[i].valid) {
                snprintf(b, sizeof(b), "%.1f \xc2\xb0" "C", ow[i].temp_c);
            } else {
                strlcpy(b, "--", sizeof(b));
            }
            set_text(s_ow_val[i], b);
        } else {
            set_text(s_ow_cap[i], "");
            set_text(s_ow_val[i], "");
        }
    }
    snprintf(b, sizeof(b), "%d/%d", page + 1, count);
    set_text(s_pageno, b);
}

// Network-down overlay on every page except the network page. Shown once the network has
// been down (no WLAN or no IPv4) for NET_DOWN_GRACE_US, so a normal boot does not flash it.
#define NET_DOWN_GRACE_US (20LL * 1000 * 1000)
static int64_t s_net_down_since = 1;   // 1 = "since boot"

static void update_overlay(void)
{
    netmgr_status_t n;
    netmgr_get_status(&n);
    int64_t now = esp_timer_get_time();
    bool up = n.associated && n.has_ip4;
    if (up) {
        s_net_down_since = 0;
    } else if (s_net_down_since == 0) {
        s_net_down_since = now;
    }
    bool show = !up && s_page != PAGE_NETWORK && now - s_net_down_since > NET_DOWN_GRACE_US;
    if (show) {
        char b[96];
        if (!n.ssid[0]) {
            strlcpy(b, "WLAN not configured - 'set wifi' on the console", sizeof(b));
        } else if (!n.associated) {
            snprintf(b, sizeof(b), "WLAN '%s' not connected", n.ssid);
        } else {
            snprintf(b, sizeof(b), "WLAN '%s' up, no IPv4 address (DHCP)", n.ssid);
        }
        char d[32];
        console_fmt_uptime(d, sizeof(d), now - s_net_down_since, false);
        char line[160];
        snprintf(line, sizeof(line), "%s\ndown for %s", b, d);
        set_text(s_ovl_text, line);
    }
    if (lv_obj_is_hidden(s_overlay) == show) {
        lv_obj_set_hidden(s_overlay, !show);
    }
}

static void page_network(char *c0, size_t l0, char *c1, size_t l1)
{
    netmgr_status_t n;
    netmgr_get_status(&n);
    set_text(s_title, "Network");
    snprintf(c0, l0, "WLAN     %s\nSSID     %s\nRSSI     %d dBm\nChannel  %u\nBSSID    " MACSTR
                     "\nMAC      " MACSTR "\nHostname %s\nReconnects %" PRIu32,
             n.associated ? "connected" : "NOT connected", n.ssid, n.associated ? n.rssi : 0, n.channel,
             MAC2STR(n.bssid), MAC2STR(n.mac), n.hostname, n.reconnects);
    c1[0] = '\0';
    if (n.has_ip4) {
        snprintf(c1, l1, "IPv4  " IPSTR "\nMask  " IPSTR "\nGW    " IPSTR "\nDNS   " IPSTR "\n", IP2STR(&n.ip4.ip),
                 IP2STR(&n.ip4.netmask), IP2STR(&n.ip4.gw), IP2STR(&n.dns4[0]));
    } else {
        snprintf(c1, l1, "IPv4  -\n");
    }
    for (int i = 0; i < n.n_ip6 && i < 3; i++) {
        char a[48];
        netmgr_ip6_str(&n.ip6[i], a, sizeof(a));
        snprintf(c1 + strlen(c1), l1 - strlen(c1), "%s\n", a);
    }
    if (n.has_ip6_ll) {
        char a[48];
        netmgr_ip6_str(&n.ip6_ll, a, sizeof(a));
        snprintf(c1 + strlen(c1), l1 - strlen(c1), "%s (link-local)\n", a);
    }
    // configured host names: resolvable? ("!" = no)
    netmgr_dns_result_t d[NETMGR_DNS_WATCH_MAX];
    int nd = netmgr_dns_results(d, NETMGR_DNS_WATCH_MAX);
    for (int i = 0; i < nd; i++) {
        if (!d[i].checked) {
            snprintf(c0 + strlen(c0), l0 - strlen(c0), "\nDNS %s %s: not checked yet", d[i].label, d[i].name);
        } else if (d[i].ok) {
            snprintf(c0 + strlen(c0), l0 - strlen(c0), "\nDNS %s %s ok", d[i].label, d[i].name);
        } else {
            snprintf(c0 + strlen(c0), l0 - strlen(c0), "\n! DNS %s %s does NOT resolve", d[i].label, d[i].name);
        }
    }
}

static void page_time(char *c0, size_t l0, char *c1, size_t l1)
{
    timesync_status_t t;
    timesync_get_status(&t);
    set_text(s_title, "Time");
    time_t now = time(NULL);
    char d[40], last[40];
    fmt_time(d, sizeof(d), "%Y-%m-%d %H:%M %Z", now);
    char off[8];
    timesync_iso_offset(now, off, sizeof(off));
    strlcat(d, " ", sizeof(d));
    strlcat(d, off, sizeof(d));
    const char *tz = getenv("TZ");
    if (t.synced) {
        fmt_time(last, sizeof(last), "%Y-%m-%d %H:%M:%S", t.last_sync);
    } else {
        strlcpy(last, "never", sizeof(last));
    }
    snprintf(c0, l0, "Time     %s\nTZ       %s\nSynced   %s\nLast     %s\nSyncs    %" PRIu32 "\nSource   %s",
             t.synced ? d : "not set", tz ? tz : "-", t.synced ? "yes" : "no", last, t.sync_count,
             timesync_source_name(t.source));
    c1[0] = '\0';
    for (int i = 0; i < TIMESYNC_SLOTS; i++) {
        if (t.server[i][0]) {
            snprintf(c1 + strlen(c1), l1 - strlen(c1), "Server %d  %s\n  reach 0x%02x\n", i, t.server[i], t.reach[i]);
        }
    }
    snprintf(c1 + strlen(c1), l1 - strlen(c1), "Fallback %s", TIMESYNC_FALLBACK);
}

typedef struct {
    const char *prefix;
    char *buf[2];
    size_t len[2];
    int n;
} metric_list_t;

static void add_metric(const metric_t *m, void *arg)
{
    metric_list_t *ml = arg;
    size_t pl = strlen(ml->prefix);
    if (strncmp(m->key, ml->prefix, pl) != 0 || m->key[pl] != '.') {
        return;
    }
    bool stale = !metrics_is_fresh(m);
    int col = ml->n < 9 ? 0 : 1;
    char *b = ml->buf[col];
    snprintf(b + strlen(b), ml->len[col] - strlen(b), "%-18s %10.2f%s\n", m->key + pl + 1, m->value,
             stale ? " (old)" : "");
    ml->n++;
}

static void page_sensor(int idx, char *c0, size_t l0, char *c1, size_t l1)
{
    sensor_info_t si[SENSORS_MAX];
    int n = sensors_list(si, SENSORS_MAX);
    if (idx >= n) {
        set_text(s_title, "Sensor");
        strlcpy(c0, "gone", l0);
        c1[0] = '\0';
        return;
    }
    const sensor_info_t *s = &si[idx];
    char t[64];
    snprintf(t, sizeof(t), "%s at %d:0x%02x%s", s->model, s->bus + 1, s->addr, s->supported ? "" : " - not supported");
    set_text(s_title, t);
    c0[0] = c1[0] = '\0';
    if (!s->supported) {
        snprintf(c0, l0, "%s\n\nDetected by its chip ID, but this firmware\nhas no driver for it yet.", s->info);
        return;
    }
    if (sensors_detail(idx, c0, l0)) {
        // a form feed splits the detail text: values left, explanations right
        char *ff = strchr(c0, '\f');
        if (ff) {
            *ff = '\0';
            strlcpy(c1, ff + 1, l1);
            return;
        }
        snprintf(c1, l1, "%s\nreads ok %" PRIu32 ", errors %" PRIu32, s->info, s->ok_count, s->err_count);
        return;
    }
    // metrics first (left, overflow right), then the driver state at the end of the right column
    metric_list_t ml = {.prefix = s->name, .buf = {c0, c1}, .len = {l0, l1}};
    metrics_foreach(add_metric, &ml);
    snprintf(c1 + strlen(c1), l1 - strlen(c1), "\n%s\nreads ok %" PRIu32 ", errors %" PRIu32, s->info, s->ok_count,
             s->err_count);
}

static void page_system(char *c0, size_t l0, char *c1, size_t l1)
{
    set_text(s_title, "System");
    const esp_app_desc_t *app = esp_app_get_description();
    char up[24];
    console_fmt_uptime(up, sizeof(up), esp_timer_get_time(), false);
    const char *rr;
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON: rr = "power-on"; break;
    case ESP_RST_SW: rr = "software"; break;
    case ESP_RST_PANIC: rr = "panic"; break;
    case ESP_RST_TASK_WDT: rr = "task watchdog"; break;
    case ESP_RST_INT_WDT: rr = "interrupt watchdog"; break;
    case ESP_RST_BROWNOUT: rr = "brownout"; break;
    case ESP_RST_EXT: rr = "external"; break;
    default: rr = "other"; break;
    }
    snprintf(c0, l0, "Firmware %s\nIDF      %s\nBoard    %s\nUptime   %s\nReset    %s\nHeap     %u kB\nPSRAM    %u kB",
             app->version, app->idf_ver, BOARD_NAME, up, rr,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    c1[0] = '\0';
    for (int b = 0; b < BOARD_I2C_BUSES; b++) {
        int sda, scl;
        gpio_num_t psda, pscl;
        board_i2c_pins(b, &psda, &pscl);
        board_i2c_idle_levels(b, &sda, &scl);
        snprintf(c1 + strlen(c1), l1 - strlen(c1), "I2C %d    GPIO%d/%d, pull-ups %s\n", b + 1, psda, pscl,
                 (sda && scl) ? "ok" : "none");
    }
    epd_stats_t e;
    epd_get_stats(&e);
    snprintf(c1 + strlen(c1), l1 - strlen(c1),
             "E-paper  %" PRIu32 " full, %" PRIu32 " partial\nLast     SPI %" PRIu32 " ms, panel %" PRIu32
             " ms\nTimeouts %" PRIu32,
             e.full_count, e.partial_count, e.last_spi_ms, e.last_refresh_ms, e.busy_timeouts);
    char w[96];
    sensors_warnings(w, sizeof(w));
    if (w[0]) {
        snprintf(c1 + strlen(c1), l1 - strlen(c1), "\nWARNING  %s", w);
    }
}

static void update_overlay(void);

static void update_page(void)
{
    update_overlay();
    int count = page_count();
    if (s_page >= count) {
        s_page = 0;
    }
    bool ov = s_page == PAGE_OVERVIEW;
    if (ov) {
        lv_obj_set_hidden(s_detail, true);
        lv_obj_set_hidden(s_overview, false);
        page_overview(s_page, count);
        return;
    }
    lv_obj_set_hidden(s_overview, true);
    lv_obj_set_hidden(s_detail, false);

    static char c0[640], c1[640];
    c0[0] = c1[0] = '\0';
    if (s_page == PAGE_NETWORK) {
        page_network(c0, sizeof(c0), c1, sizeof(c1));
    } else if (s_page == PAGE_TIME) {
        page_time(c0, sizeof(c0), c1, sizeof(c1));
    } else if (s_page == count - 1) {
        page_system(c0, sizeof(c0), c1, sizeof(c1));
    } else {
        page_sensor(s_page - PAGE_TIME - 1, c0, sizeof(c0), c1, sizeof(c1));
    }
    set_text(s_col[0], c0);
    set_text(s_col[1], c1);
    char b[16];
    snprintf(b, sizeof(b), "%d/%d", s_page + 1, count);
    set_text(s_pageno2, b);
}

// --- LVGL glue ---

static uint32_t tick_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    // I1 buffers start with an 8-byte palette (2 x lv_color32_t)
    memcpy(s_frame, px_map + 8, s_stride * EPD_HEIGHT);
    s_frame_new = true;
    lv_display_flush_ready(disp);
}

static void render_and_show(void)
{
    s_frame_new = false;
    lv_refr_now(s_disp);
    bool full = s_force_full || s_partials_since_full >= UI_FULL_EVERY;
    if (!s_frame_new && !full) {
        return;
    }
    if (!full && memcmp(s_frame, s_shown, s_stride * EPD_HEIGHT) == 0) {
        return;   // nothing visible changed
    }
    if (epd_show(s_frame, s_stride, full) == ESP_OK) {
        memcpy(s_shown, s_frame, s_stride * EPD_HEIGHT);
        s_partials_since_full = full ? 0 : s_partials_since_full + 1;
        s_force_full = false;
    }
}

// --- events ---

static void on_app_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    ui_msg_t m = {.type = MSG_REDRAW};
    if (id == APP_EVENT_KEY) {
        m.type = MSG_KEY;
        m.key = *(const app_key_event_t *)data;
    }
    xQueueSend(s_q, &m, 0);
}

static void handle_key(const app_key_event_t *k)
{
    int count = page_count();
    switch (k->key) {
    case APP_KEY_DOWN:
        s_page = (s_page + 1) % count;
        break;
    case APP_KEY_UP:
        s_page = (s_page + count - 1) % count;
        break;
    case APP_KEY_MENU:
    case APP_KEY_EXIT:
        s_page = PAGE_OVERVIEW;
        break;
    case APP_KEY_OK:
        s_force_full = true;   // manual "clean the screen"
        break;
    case APP_KEY_BOOT:
        if (k->long_press) {
            sensors_scd4x_fresh_air_start();   // "I am in fresh air"
        }
        break;
    }
    ESP_LOGI(TAG, "event=key key=%d long=%d page=%d pages=%d", k->key, k->long_press, s_page + 1, count);
}

// Display refresh: at every boundary of the display interval (60 s = start of each minute,
// aligned to UTC once NTP synced), REFRESH_AFTER_US after it - the clock has turned and the
// sensors without a cadence finished 100 ms before the boundary.
#define REFRESH_AFTER_US (20LL * 1000)
static int64_t s_last_boundary_us;

static int64_t next_boundary_us(void)
{
    appcfg_t c;
    appcfg_get(&c);
    int64_t b = sched_next_us(c.display_s, esp_timer_get_time() - REFRESH_AFTER_US);
    if (b <= s_last_boundary_us) {
        b = sched_next_us(c.display_s, s_last_boundary_us);   // woke a tick early: not this one again
    }
    return b;
}

static TickType_t ticks_until(int64_t t_us)
{
    int64_t us = t_us - esp_timer_get_time();
    if (us <= 0) {
        return 0;
    }
    int64_t tick_us = portTICK_PERIOD_MS * 1000LL;
    return (TickType_t)((us + tick_us - 1) / tick_us);   // rounded up: never before t_us
}

static void ui_task(void *arg)
{
    update_page();
    render_and_show();
    for (;;) {
        ui_msg_t m;
        int64_t boundary = next_boundary_us();
        if (xQueueReceive(s_q, &m, ticks_until(boundary + REFRESH_AFTER_US)) != pdTRUE) {
            // interval boundary: the tiles show the means of the interval that just ended
            s_last_boundary_us = boundary;
            metrics_close_window(METRICS_WIN_DISPLAY);
        } else {
            if (m.type == MSG_KEY) {
                handle_key(&m.key);
            } else {
                // let a burst (associated, IPv4, IPv6, NTP) settle before refreshing the panel
                TickType_t until = xTaskGetTickCount() + pdMS_TO_TICKS(UI_EVENT_SETTLE_MS);
                while (xQueueReceive(s_q, &m, until - xTaskGetTickCount()) == pdTRUE) {
                    if (m.type == MSG_KEY) {
                        handle_key(&m.key);
                        break;
                    }
                    if ((int32_t)(until - xTaskGetTickCount()) <= 0) {
                        break;
                    }
                }
            }
        }
        update_page();
        render_and_show();
    }
}

// "screen [scale]": the frame last sent to the panel as ASCII art, one char per scale x scale
// pixels ('#' = any black pixel). Lets you check the layout without looking at the panel.
static int cmd_screen(int argc, char **argv)
{
    int sc = argc > 1 ? atoi(argv[1]) : 4;
    if (sc < 1 || sc > 16) {
        sc = 4;
    }
    for (int y = 0; y < EPD_HEIGHT; y += sc) {
        char line[EPD_WIDTH + 2];
        int n = 0;
        for (int x = 0; x < EPD_WIDTH; x += sc) {
            bool black = false;
            for (int dy = 0; dy < sc && y + dy < EPD_HEIGHT && !black; dy++) {
                for (int dx = 0; dx < sc && x + dx < EPD_WIDTH; dx++) {
                    int px = x + dx;
                    if (!(s_shown[(y + dy) * s_stride + px / 8] & (0x80 >> (px % 8)))) {
                        black = true;
                        break;
                    }
                }
            }
            line[n++] = black ? '#' : '.';
        }
        line[n] = '\0';
        printf("%s\n", line);
    }
    return 0;
}

esp_err_t ui_start(void)
{
    lv_init();
    lv_tick_set_cb(tick_ms);

    s_disp = lv_display_create(EPD_WIDTH, EPD_HEIGHT);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_I1);
    s_stride = lv_draw_buf_width_to_stride(EPD_WIDTH, LV_COLOR_FORMAT_I1);
    size_t buf_size = s_stride * EPD_HEIGHT + 8;
    void *buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    s_frame = heap_caps_malloc(s_stride * EPD_HEIGHT, MALLOC_CAP_SPIRAM);
    s_shown = heap_caps_calloc(1, s_stride * EPD_HEIGHT, MALLOC_CAP_SPIRAM);
    if (!buf || !s_frame || !s_shown) {
        return ESP_ERR_NO_MEM;
    }
    lv_display_set_buffers(s_disp, buf, NULL, buf_size, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(s_disp, flush_cb);
    ESP_LOGI(TAG, "event=ready lvgl=%d.%d.%d width=%d height=%d format=I1 stride=%d", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH,
             EPD_WIDTH, EPD_HEIGHT, s_stride);

    build_screen();

    s_q = xQueueCreate(8, sizeof(ui_msg_t));
    esp_event_handler_register(APP_EVENT, ESP_EVENT_ANY_ID, on_app_event, NULL);
    // LVGL rendering + 1 bpp blending: generous stack
    xTaskCreate(ui_task, "ui", 8192, NULL, 3, NULL);
    const esp_console_cmd_t c = {.command = "screen", .help = "screen [scale]: print the panel content", .func = cmd_screen};
    esp_console_cmd_register(&c);
    return ESP_OK;
}
