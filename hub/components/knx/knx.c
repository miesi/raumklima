#include "knx.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "app_events.h"
#include "appcfg.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "health.h"
#include "knx_proto.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "netmgr.h"
#include "nvs.h"
#include "sched.h"

static const char *TAG = "knx";

#define NVS_NS "knx"
#define NVS_KEY "ga"
#define QUEUE_LEN 128
// after the boundary: the sensors without a cadence finished 100 ms before it, display (+20 ms)
// and Carbon (+50 ms) closed their windows
#define AFTER_US (80LL * 1000)
#define BUSY_EXTRA_MS 50
#define SEARCH_MAX 4

// NVS layout of one group address (version 1)
typedef struct {
    uint16_t ga;
    uint16_t dpt;
    uint8_t dir;
    char name[KNX_NAME_LEN];
} stored_t;

typedef struct {
    uint16_t ga;
    uint16_t apci;
    uint8_t data[2];
} tx_t;

// DPT 9 subtypes: unit (VERIFIED: calimero 2.6 DPTXlator2ByteFloat) and the metric unit
// suffix that fits; scale converts the metric value to the DPT unit.
typedef struct {
    uint16_t dpt;
    const char *unit;        // for the display (UTF-8 escapes, the fonts have the degree sign)
    const char *suffix;      // metric key suffix, NULL = no metric has this unit
    double scale;
} dpt_info_t;

static const dpt_info_t DPTS[] = {
    {1, "\xc2\xb0" "C", "_c", 1},     {2, "K", NULL, 1},      {4, "lx", "_lx", 1},
    {5, "m/s", NULL, 1},              {6, "hPa", "_hpa", 100}, // metric hPa, DPT Pa; shown as hPa
    {7, "%", "_pct", 1},              {8, "ppm", "_ppm", 1},  {10, "s", NULL, 1},
    {11, "ms", NULL, 1},              {20, "mV", NULL, 1},    {21, "mA", NULL, 1},
    {24, "kW", NULL, 1},              {25, "l/h", NULL, 1},   {26, "l/m\xc2\xb2", NULL, 1},
    {27, "\xc2\xb0" "F", NULL, 1},    {28, "km/h", NULL, 1},
};

static SemaphoreHandle_t s_lock;   // s_ga, s_n, s_q, s_st
static knx_ga_info_t s_ga[KNX_GA_MAX];
static int s_n;
static tx_t s_q[QUEUE_LEN];
static int s_qhead, s_qlen;
static knx_status_t s_st;
static TaskHandle_t s_task;
static volatile bool s_replan;
static int64_t s_next_boundary_us;   // planned boundary (esp_timer), 0 = plan from now
static uint32_t s_min_count;
static volatile int64_t s_monitor_until;
static volatile uint32_t s_test_target;   // 0 = multicast
static int64_t s_min_start_us;

static const dpt_info_t *dpt_info(uint16_t dpt)
{
    for (size_t i = 0; i < sizeof(DPTS) / sizeof(DPTS[0]); i++) {
        if (DPTS[i].dpt == dpt) {
            return &DPTS[i];
        }
    }
    return NULL;
}

const char *knx_dpt_unit(uint16_t dpt)
{
    const dpt_info_t *d = dpt_info(dpt);
    return d ? d->unit : "";
}

double knx_display_value(uint16_t dpt, double v)
{
    return dpt == 6 ? v / 100.0 : v;   // Pa on the bus, hPa on the display
}

// --- table and NVS ---

static void save_locked(void)
{
    static stored_t st[KNX_GA_MAX];
    for (int i = 0; i < s_n; i++) {
        st[i].ga = s_ga[i].ga;
        st[i].dpt = s_ga[i].dpt;
        st[i].dir = (uint8_t)s_ga[i].dir;
        strlcpy(st[i].name, s_ga[i].name, sizeof(st[i].name));
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = s_n ? nvs_set_blob(h, NVS_KEY, st, s_n * sizeof(stored_t)) : nvs_erase_key(h, NVS_KEY);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "event=save_failed err=%s", esp_err_to_name(err));
    }
}

static void load(void)
{
    static stored_t st[KNX_GA_MAX];
    size_t len = sizeof(st);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    if (nvs_get_blob(h, NVS_KEY, st, &len) == ESP_OK) {
        s_n = (int)(len / sizeof(stored_t));
        for (int i = 0; i < s_n; i++) {
            memset(&s_ga[i], 0, sizeof(s_ga[i]));
            s_ga[i].ga = st[i].ga;
            s_ga[i].dpt = st[i].dpt;
            s_ga[i].dir = st[i].dir ? KNX_LISTEN : KNX_SEND;
            strlcpy(s_ga[i].name, st[i].name, sizeof(s_ga[i].name));
        }
    }
    nvs_close(h);
}

static int find_locked(uint16_t ga)
{
    for (int i = 0; i < s_n; i++) {
        if (s_ga[i].ga == ga) {
            return i;
        }
    }
    return -1;
}

static bool ends_with(const char *s, const char *suffix)
{
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && strcmp(s + a - b, suffix) == 0;
}

esp_err_t knx_ga_add(uint16_t ga, uint16_t dpt, knx_dir_t dir, const char *name, char *msg, size_t msg_len)
{
    msg[0] = '\0';
    const dpt_info_t *d = dpt_info(dpt);
    if (!d) {
        snprintf(msg, msg_len, "DPT 9.%03u is not known here; known: 9.001 9.002 9.004 9.005 9.006 9.007 9.008 ...",
                 dpt);
        return ESP_ERR_INVALID_ARG;
    }
    if (dir == KNX_SEND) {
        if (!name || !name[0]) {
            snprintf(msg, msg_len, "send needs a metric key, e.g. sht4x.temperature_c ('show status' lists them)");
            return ESP_ERR_INVALID_ARG;
        }
        // a metric with a known unit must go to the DPT with that unit
        for (size_t i = 0; i < sizeof(DPTS) / sizeof(DPTS[0]); i++) {
            if (DPTS[i].suffix && ends_with(name, DPTS[i].suffix) && DPTS[i].dpt != dpt) {
                snprintf(msg, msg_len, "%s has the unit of 9.%03u (%s), not 9.%03u", name, DPTS[i].dpt, DPTS[i].unit,
                         dpt);
                return ESP_ERR_INVALID_ARG;
            }
        }
        metric_t m;
        if (!metrics_get(name, &m)) {
            snprintf(msg, msg_len, "note: no fresh metric %s at this time; it is sent when it has readings", name);
        } else if (!d->suffix || !ends_with(name, d->suffix)) {
            snprintf(msg, msg_len, "note: no unit check for %s, the value goes out as it is", name);
        }
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int i = find_locked(ga);
    if (i < 0) {
        if (s_n >= KNX_GA_MAX) {
            xSemaphoreGive(s_lock);
            snprintf(msg, msg_len, "table full (%d group addresses)", KNX_GA_MAX);
            return ESP_ERR_NO_MEM;
        }
        i = s_n++;
    }
    memset(&s_ga[i], 0, sizeof(s_ga[i]));
    s_ga[i].ga = ga;
    s_ga[i].dpt = dpt;
    s_ga[i].dir = dir;
    strlcpy(s_ga[i].name, name ? name : "", sizeof(s_ga[i].name));
    save_locked();
    xSemaphoreGive(s_lock);
    char g[16];
    knx_ga_format(ga, g, sizeof(g));
    ESP_LOGI(TAG, "event=ga_added ga=%s dpt=9.%03u dir=%s name=%s", g, dpt, dir == KNX_SEND ? "send" : "listen",
             name && name[0] ? name : "-");
    return ESP_OK;
}

esp_err_t knx_ga_del(uint16_t ga)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int i = find_locked(ga);
    if (i >= 0) {
        memmove(&s_ga[i], &s_ga[i + 1], (s_n - i - 1) * sizeof(s_ga[0]));
        s_n--;
        save_locked();
    }
    xSemaphoreGive(s_lock);
    return i >= 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

int knx_ga_list(knx_ga_info_t *out, int max)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_n < max ? s_n : max;
    memcpy(out, s_ga, n * sizeof(s_ga[0]));
    xSemaphoreGive(s_lock);
    return n;
}

void knx_get_status(knx_status_t *out)
{
    appcfg_t c;
    appcfg_get(&c);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_st;
    out->queued = s_qlen;
    xSemaphoreGive(s_lock);
    out->on = c.knx_on;
    out->primary = c.role == APPCFG_ROLE_PRIMARY;
    out->ia = c.knx_ia;
    out->interval_s = c.knx_s;
}

void knx_reschedule(void)
{
    s_replan = true;
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

// --- send queue ---

static void enqueue_locked(uint16_t ga, uint16_t apci, const uint8_t d[2])
{
    if (s_qlen >= QUEUE_LEN) {
        s_st.tx_dropped++;
        return;
    }
    tx_t *t = &s_q[(s_qhead + s_qlen) % QUEUE_LEN];
    t->ga = ga;
    t->apci = apci;
    memcpy(t->data, d, 2);
    s_qlen++;
}

// Value of a metric for KNX: the mean of the closed KNX window; a state: its last value.
// last_ok: without a closed window (before the first boundary) the last reading - for read
// responses only, the writes always send a mean.
static bool metric_value(const char *key, double *v, bool last_ok)
{
    metric_t m;
    if (!metrics_get(key, &m)) {
        return false;   // missing or stale
    }
    if (m.state) {
        *v = m.value;
        return true;
    }
    if (m.win[METRICS_WIN_KNX].n == 0) {
        if (last_ok) {
            *v = m.value;
            return true;
        }
        return false;   // no reading in the interval
    }
    *v = m.win[METRICS_WIN_KNX].mean;
    return true;
}

static bool encode_for(const knx_ga_info_t *g, double v, uint8_t d[2])
{
    const dpt_info_t *di = dpt_info(g->dpt);
    return dpt9_encode(v * (di ? di->scale : 1), d);
}

// At the KNX boundary (primary only): one GroupValueWrite for each send address.
static void queue_writes(void)
{
    int queued = 0, skipped = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_n; i++) {
        knx_ga_info_t *g = &s_ga[i];
        if (g->dir != KNX_SEND) {
            continue;
        }
        double v;
        uint8_t d[2];
        if (!metric_value(g->name, &v, false) || !encode_for(g, v, d)) {
            skipped++;
            continue;
        }
        enqueue_locked(g->ga, KNX_APCI_WRITE, d);
        g->sent = true;
        g->sent_value = v;
        g->sent_us = esp_timer_get_time();
        queued++;
    }
    xSemaphoreGive(s_lock);
    if (queued || skipped) {
        ESP_LOGI(TAG, "event=interval queued=%d skipped=%d gap_ms=%d", queued, skipped, KNX_TX_GAP_MS);
    }
}

// --- receive ---

static void on_telegram(const knx_msg_t *m, const appcfg_t *c)
{
    if (m->src == c->knx_ia) {
        return;   // our own (multicast loop) - or a device with the same address
    }
    int64_t now = esp_timer_get_time();
    if (now < s_monitor_until) {
        char src[16], dst[16], val[24] = "";
        knx_ia_format(m->src, src, sizeof(src));
        if (m->group) {
            knx_ga_format(m->dst, dst, sizeof(dst));
        } else {
            knx_ia_format(m->dst, dst, sizeof(dst));
        }
        double v;
        if (m->dlen == 2 && dpt9_decode(m->data, &v)) {
            snprintf(val, sizeof(val), " as_dpt9=%.2f", v);
        }
        char data[2 * sizeof(m->data) + 1] = "";
        for (int i = 0; i < m->dlen; i++) {
            sprintf(data + 2 * i, "%02x", m->data[i]);
        }
        ESP_LOGI(TAG, "event=rx src=%s dst=%s apci=%s data=%s%s", src, dst,
                 m->apci == KNX_APCI_READ ? "read" : m->apci == KNX_APCI_RESPONSE ? "response"
                 : m->apci == KNX_APCI_WRITE ? "write" : "other", m->dlen ? data : "-", val);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.rx++;
    s_st.last_rx_us = now;
    s_min_count++;
    int i = m->group ? find_locked(m->dst) : -1;
    if (i >= 0) {
        knx_ga_info_t *g = &s_ga[i];
        g->telegrams++;
        g->last_rx_us = now;
        g->last_src = m->src;
        if (g->dir == KNX_LISTEN && (m->apci == KNX_APCI_WRITE || m->apci == KNX_APCI_RESPONSE) && m->dlen == 2) {
            g->has_value = dpt9_decode(m->data, &g->value);
        } else if (g->dir == KNX_SEND && m->apci == KNX_APCI_READ && c->role == APPCFG_ROLE_PRIMARY) {
            // answer with the value of the last interval, before the first one with the current mean
            double v = 0;
            uint8_t d[2];
            if ((g->sent || metric_value(g->name, &v, true)) && encode_for(g, g->sent ? g->sent_value : v, d)) {
                enqueue_locked(g->ga, KNX_APCI_RESPONSE, d);
            }
        }
    }
    xSemaphoreGive(s_lock);
}

// --- search (console "knx search") ---

static volatile bool s_search_req;
static knx_msg_t s_found[SEARCH_MAX];
static char s_found_ip[SEARCH_MAX][16];
static int s_nfound;

void knx_set_test_target(uint32_t ip)
{
    s_test_target = ip;
    if (ip) {
        struct in_addr a = {.s_addr = ip};
        ESP_LOGW(TAG, "event=test_target ip=%s hint=\"telegrams go unicast, not to the KNX line\"", inet_ntoa(a));
    } else {
        ESP_LOGI(TAG, "event=test_target ip=none");
    }
}

uint32_t knx_test_target(void)
{
    return s_test_target;
}

void knx_monitor(int seconds)
{
    s_monitor_until = seconds > 0 ? esp_timer_get_time() + seconds * 1000000LL : 0;
}

void knx_search_start(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_nfound = 0;
    xSemaphoreGive(s_lock);
    s_search_req = true;
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

int knx_search_results(char (*lines)[128], int max)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_nfound < max ? s_nfound : max;
    for (int i = 0; i < n; i++) {
        char ia[16];
        knx_ia_format(s_found[i].dev_ia, ia, sizeof(ia));
        snprintf(lines[i], 128, "%s  name '%s'  address %s  routing %u.%u.%u.%u", s_found_ip[i], s_found[i].dev_name,
                 ia, s_found[i].dev_mcast[0], s_found[i].dev_mcast[1], s_found[i].dev_mcast[2],
                 s_found[i].dev_mcast[3]);
    }
    xSemaphoreGive(s_lock);
    return n;
}

// --- task ---

static void set_error(const char *what)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_st.error, sizeof(s_st.error), "%s: %s", what, strerror(errno));
    xSemaphoreGive(s_lock);
    ESP_LOGW(TAG, "event=socket_error what=%s err=\"%s\"", what, strerror(errno));
}

static int open_socket(uint32_t my_ip)
{
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) {
        set_error("socket");
        return -1;
    }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(KNX_PORT), .sin_addr.s_addr = htonl(INADDR_ANY)};
    struct ip_mreq mr = {.imr_multiaddr.s_addr = inet_addr(KNX_MCAST), .imr_interface.s_addr = my_ip};
    struct in_addr ifa = {.s_addr = my_ip};
    uint8_t loop = 0;
    struct timeval tv = {.tv_sec = 0, .tv_usec = 100 * 1000};
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0) {
        set_error("bind");
    } else if (setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, sizeof(mr)) != 0) {
        set_error("join");
    } else {
        setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, &ifa, sizeof(ifa));
        setsockopt(s, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_st.error[0] = '\0';
        s_st.running = true;
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "event=joined group=%s port=%d", KNX_MCAST, KNX_PORT);
        return s;
    }
    close(s);
    return -1;
}

static void close_socket(int *s)
{
    if (*s >= 0) {
        close(*s);
        *s = -1;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.running = false;
    xSemaphoreGive(s_lock);
}

// The boundary after the one just handled. + 0.5 s: an NTP correction must not give the same
// boundary again (as in carbon.c). Missed boundaries are skipped, KNX does not catch up.
static int64_t next_boundary(int iv, int64_t done)
{
    int64_t b = sched_next_us(iv, done + 500000);
    int64_t now = esp_timer_get_time();
    return b < now ? sched_next_us(iv, now) : b;
}

static void on_time_synced(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    knx_reschedule();   // the boundaries follow the UTC clock from now on
}

static void knx_task(void *arg)
{
    health_id_t hid = health_register("knx");
    int sock = -1;
    uint32_t sock_ip = 0;
    int64_t next_tx = 0, next_check = 0;
    static uint8_t pkt[256];
    for (;;) {
        health_alive(hid, 1000000);
        appcfg_t c;
        appcfg_get(&c);
        int64_t now = esp_timer_get_time();
        if (s_replan) {
            s_replan = false;
            s_next_boundary_us = 0;
        }
        if (now >= next_check) {
            next_check = now + 5000000;
            netmgr_status_t n;
            netmgr_get_status(&n);
            uint32_t ip = n.has_ip4 ? n.ip4.ip.addr : 0;
            if (!c.knx_on || ip != sock_ip) {
                close_socket(&sock);
                sock_ip = 0;
            }
            if (c.knx_on && ip && sock < 0) {
                sock = open_socket(ip);
                sock_ip = sock >= 0 ? ip : 0;
            }
        }
        if (sock < 0) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
            continue;
        }

        // wait for a packet: at most 100 ms, and with telegrams queued not past the next send
        // slot, so that the gap is KNX_TX_GAP_MS and not up to 100 ms more
        int64_t wait_us = 100000;
        if (s_qlen && next_tx - now < wait_us) {
            wait_us = next_tx > now ? next_tx - now : 0;
        }
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(sock, &rf);
        struct timeval wt = {.tv_sec = 0, .tv_usec = (suseconds_t)wait_us};
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int r = -1;
        if (select(sock + 1, &rf, NULL, NULL, &wt) > 0) {
            r = recvfrom(sock, pkt, sizeof(pkt), MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
        }
        if (r > 0) {
            knx_msg_t m;
            if (knx_parse(pkt, (size_t)r, &m)) {
                if (m.ldata) {
                    on_telegram(&m, &c);
                } else if (m.service == KNX_SVC_ROUTING_BUSY) {
                    // spec: wait the time the router asks for, plus a random part (ASSUMPTION:
                    // 0..50 ms is enough for one hub)
                    next_tx = esp_timer_get_time() + (m.busy_wait_ms + (esp_random() % BUSY_EXTRA_MS)) * 1000LL;
                    xSemaphoreTake(s_lock, portMAX_DELAY);
                    s_st.busy++;
                    xSemaphoreGive(s_lock);
                    ESP_LOGW(TAG, "event=routing_busy wait_ms=%u", m.busy_wait_ms);
                } else if (m.service == KNX_SVC_SEARCH_RES) {
                    xSemaphoreTake(s_lock, portMAX_DELAY);
                    if (s_nfound < SEARCH_MAX) {
                        s_found[s_nfound] = m;
                        inet_ntoa_r(from.sin_addr, s_found_ip[s_nfound], sizeof(s_found_ip[0]));
                        s_nfound++;
                    }
                    xSemaphoreGive(s_lock);
                    ESP_LOGI(TAG, "event=search_response peer=%s name=\"%s\"", inet_ntoa(from.sin_addr), m.dev_name);
                } else if (m.service == KNX_SVC_ROUTING_LOST) {
                    ESP_LOGW(TAG, "event=routing_lost_message");
                }
            }
        }
        now = esp_timer_get_time();

        // bus traffic: telegrams in the last full minute
        if (now - s_min_start_us >= 60000000) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_st.rx_last_min = s_min_start_us ? s_min_count : 0;
            xSemaphoreGive(s_lock);
            s_min_count = 0;
            s_min_start_us = now;
        }

        // interval boundary: plan it once, then wait for it. (Planning it again in each pass
        // from "now" never fires: 80 ms after the boundary, "the next one" is already the
        // following boundary.)
        if (!s_next_boundary_us) {
            s_next_boundary_us = sched_next_us(c.knx_s, now);
        }
        if (now >= s_next_boundary_us + AFTER_US) {
            int64_t b = s_next_boundary_us;
            metrics_close_window(METRICS_WIN_KNX);
            if (c.role == APPCFG_ROLE_PRIMARY) {
                queue_writes();
            }
            s_next_boundary_us = next_boundary(c.knx_s, b);
        }

        // search request (IP side only)
        if (s_search_req) {
            s_search_req = false;
            uint8_t q[16];
            size_t ql = knx_build_search(q, sizeof(q), (const uint8_t *)&sock_ip, KNX_PORT);
            struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(KNX_PORT),
                                     .sin_addr.s_addr = inet_addr(KNX_MCAST)};
            if (sendto(sock, q, ql, 0, (struct sockaddr *)&to, sizeof(to)) < 0) {
                set_error("search");
            }
        }

        // one telegram per gap
        if (now >= next_tx) {
            tx_t t;
            bool have = false;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (s_qlen) {
                t = s_q[s_qhead];
                s_qhead = (s_qhead + 1) % QUEUE_LEN;
                s_qlen--;
                have = true;
            }
            xSemaphoreGive(s_lock);
            if (have) {
                uint8_t f[32];
                size_t fl2 = knx_build_group(f, sizeof(f), c.knx_ia, t.ga, t.apci, t.data, 2);
                uint32_t tt = s_test_target;
                struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(KNX_PORT),
                                         .sin_addr.s_addr = tt ? tt : inet_addr(KNX_MCAST)};
                char g[16];
                knx_ga_format(t.ga, g, sizeof(g));
                if (sendto(sock, f, fl2, 0, (struct sockaddr *)&to, sizeof(to)) < 0) {
                    set_error("send");
                } else {
                    xSemaphoreTake(s_lock, portMAX_DELAY);
                    if (t.apci == KNX_APCI_RESPONSE) {
                        s_st.tx_response++;
                    } else {
                        s_st.tx_write++;
                    }
                    xSemaphoreGive(s_lock);
                    double v = 0;
                    dpt9_decode(t.data, &v);
                    ESP_LOGD(TAG, "event=sent ga=%s apci=%s data=%02x%02x value=%.2f test_target=%d", g,
                             t.apci == KNX_APCI_RESPONSE ? "response" : "write", t.data[0], t.data[1], v, tt != 0);
                }
                next_tx = esp_timer_get_time() + KNX_TX_GAP_MS * 1000LL;   // from the real send time
            }
        }
    }
}

esp_err_t knx_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    load();
    appcfg_t c;
    appcfg_get(&c);
    char ia[16];
    knx_ia_format(c.knx_ia, ia, sizeof(ia));
    ESP_LOGI(TAG, "event=loaded group_addresses=%d address=%s interval_s=%u on=%d role=%s", s_n, ia, c.knx_s,
             c.knx_on, c.role == APPCFG_ROLE_PRIMARY ? "primary" : "secondary");
    if (c.knx_ia == APPCFG_KNX_IA_DEFAULT) {
        ESP_LOGW(TAG, "event=default_address address=%s hint=\"set it with knx address <a.l.d>\"", ia);
    }
    esp_event_handler_register(APP_EVENT, APP_EVENT_TIME_SYNCED, on_time_synced, NULL);
    return xTaskCreate(knx_task, "knx", 4096, NULL, 3, &s_task) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
