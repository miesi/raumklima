#include "carbon.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

#include "appcfg.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "health.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "metrics.h"
#include "netlog.h"
#include "netmgr.h"
#include "sched.h"
#include "sensors.h"
#include "timesync.h"

static const char *TAG = "carbon";

#define AFTER_US (50LL * 1000)              // after the boundary: sensors done, display closed its window
#define BUF_BYTES (2 * 1024 * 1024)         // PSRAM: hours of batches while the receiver is down
#define BATCH_MAX (64 * 1024)
#define CONNECT_TIMEOUT_MS 3000

typedef struct {
    int64_t mono_us;   // the boundary; converted to UTC when sent
    uint32_t len;      // bytes of "path value\n" lines that follow
} batch_hdr_t;

// Static in PSRAM, not from the heap: a core dump with CONFIG_ESP_COREDUMP_CAPTURE_DRAM copies
// every used heap block incl. PSRAM, and 2 MB do not fit the coredump partition. External
// .bss is not part of the dump.
EXT_RAM_BSS_ATTR static char s_buf_mem[BUF_BYTES];
EXT_RAM_BSS_ATTR static char s_build_mem[BATCH_MAX];
static char *const s_buf = s_buf_mem;       // batch_hdr_t + lines, oldest first
static size_t s_len;
static char *const s_build = s_build_mem;   // one batch while it is built
static SemaphoreHandle_t s_lock;            // s_buf, s_len and the status
static TaskHandle_t s_task;
static carbon_status_t s_st;
static int64_t s_last_tick_us, s_last_carbon_us;
static int64_t s_retry_at;
static bool s_first_window_done;   // the window from boot to the first boundary is not sent

void carbon_reschedule(void)
{
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

void carbon_get_status(carbon_status_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_st;
    out->buffered_bytes = s_len;
    uint32_t n = 0;
    for (size_t o = 0; o + sizeof(batch_hdr_t) <= s_len;) {
        const batch_hdr_t *h = (const batch_hdr_t *)(s_buf + o);
        o += sizeof(*h) + h->len;
        n++;
    }
    out->buffered_batches = n;
    xSemaphoreGive(s_lock);
    appcfg_t c;
    appcfg_get(&c);
    out->enabled = c.carbon_host[0] != '\0';
    out->waiting_clock = out->enabled && !sched_clock_valid();
}

// --- self monitoring ---

static void publish_self(void)
{
    metrics_publish_state("hub.uptime_s", esp_timer_get_time() / 1000000);
    metrics_publish_state("hub.reset_reason", esp_reset_reason());
    metrics_publish("hub.heap_free_bytes", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    metrics_publish_state("hub.heap_min_free_bytes", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    metrics_publish("hub.psram_free_bytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    netmgr_status_t ns;
    netmgr_get_status(&ns);
    if (ns.associated) {
        metrics_publish("hub.wifi_rssi_dbm", ns.rssi);
    }
    metrics_publish_state("hub.wifi_reconnects", ns.reconnects);
    timesync_status_t ts;
    timesync_get_status(&ts);
    metrics_publish_state("hub.ntp_syncs", ts.sync_count);
    if (ts.offset_valid) {
        metrics_publish_state("hub.ntp_offset_ms", ts.offset_us / 1000.0f);
    }
    carbon_status_t cs;
    carbon_get_status(&cs);
    metrics_publish_state("hub.carbon_errors", cs.errors);
    metrics_publish_state("hub.carbon_dropped_batches", cs.dropped_batches);
    metrics_publish("hub.carbon_buffered_bytes", cs.buffered_bytes);
    netlog_status_t ls;
    netlog_get_status(&ls);
    metrics_publish_state("hub.syslog_errors", ls.errors);
    metrics_publish_state("hub.syslog_dropped", ls.dropped);
    metrics_publish("hub.syslog_buffered", ls.buffered);
    metrics_publish_state("hub.sensor_cycle_ms", sensors_read_ms());
    sensor_info_t si[SENSORS_MAX];
    int n = sensors_list(si, SENSORS_MAX);
    for (int i = 0; i < n; i++) {
        if (si[i].supported) {
            char key[METRIC_KEY_LEN];
            snprintf(key, sizeof(key), "%s.read_errors", si[i].name);
            metrics_publish_state(key, si[i].err_count);
        }
    }
}

// --- batches ---

typedef struct {
    const appcfg_t *c;
    size_t len;
    bool full;
} build_t;

static void add_line(build_t *b, const char *key, const char *suffix, float v)
{
    int r = snprintf(s_build + b->len, BATCH_MAX - b->len, "%s.%s.%s%s %.7g\n", b->c->carbon_prefix, b->c->hostname, key,
                     suffix, v);
    if (r < 0 || b->len + r >= BATCH_MAX) {
        b->full = true;
        return;
    }
    b->len += r;
}

static void add_metric(const metric_t *m, void *arg)
{
    build_t *b = arg;
    const metric_win_t *w = &m->win[METRICS_WIN_CARBON];
    if (w->n == 0 || b->full) {
        return;   // no reading in this interval: nothing to send (a pulled sensor)
    }
    if (m->state) {
        add_line(b, m->key, "", m->value);
        return;
    }
    add_line(b, m->key, "", w->mean);
    if (w->n > 1) {
        add_line(b, m->key, "_min", w->min);
        add_line(b, m->key, "_max", w->max);
    }
}

static void buffer_batch(int64_t boundary_us, const appcfg_t *c)
{
    build_t b = {.c = c};
    metrics_foreach(add_metric, &b);
    if (b.full) {
        ESP_LOGW(TAG, "event=batch_truncated bytes=%u", (unsigned)b.len);
    }
    if (b.len == 0) {
        return;
    }
    size_t need = sizeof(batch_hdr_t) + b.len;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t dropped = 0;
    while (s_len + need > BUF_BYTES && s_len > 0) {   // drop the oldest batches
        const batch_hdr_t *h = (const batch_hdr_t *)s_buf;
        size_t first = sizeof(*h) + h->len;
        memmove(s_buf, s_buf + first, s_len - first);
        s_len -= first;
        dropped++;
    }
    s_st.dropped_batches += dropped;
    batch_hdr_t h = {.mono_us = boundary_us, .len = b.len};
    memcpy(s_buf + s_len, &h, sizeof(h));
    memcpy(s_buf + s_len + sizeof(h), s_build, b.len);
    s_len += need;
    xSemaphoreGive(s_lock);
    if (dropped) {
        ESP_LOGW(TAG, "event=buffer_full dropped_batches=%lu", (unsigned long)dropped);
    }
}

// --- sending ---

static void set_error(const char *what, int err)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.errors++;
    snprintf(s_st.last_error, sizeof(s_st.last_error), "%s: %s", what, err ? strerror(err) : "failed");
    xSemaphoreGive(s_lock);
}

static int connect_to(const struct addrinfo *ai)
{
    int s = socket(ai->ai_family, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) {
        return -1;
    }
    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, fl | O_NONBLOCK);
    int r = connect(s, ai->ai_addr, ai->ai_addrlen);
    if (r < 0 && errno != EINPROGRESS) {
        close(s);
        return -1;
    }
    if (r < 0) {
        fd_set w;
        FD_ZERO(&w);
        FD_SET(s, &w);
        struct timeval to = {.tv_sec = CONNECT_TIMEOUT_MS / 1000};
        int err = 0;
        socklen_t el = sizeof(err);
        if (select(s + 1, NULL, &w, NULL, &to) != 1 || getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el) < 0 || err) {
            errno = err ? err : ETIMEDOUT;
            close(s);
            return -1;
        }
    }
    fcntl(s, F_SETFL, fl);
    struct timeval snd = {.tv_sec = 5};
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof(snd));
    return s;
}

static bool send_all(int sock, const char *p, size_t n)
{
    while (n) {
        int r = send(sock, p, n, 0);
        if (r <= 0) {
            return false;
        }
        p += r;
        n -= r;
    }
    return true;
}

// one batch: every "path value" line gets " <utc>"
static bool send_batch(int sock, const batch_hdr_t *h, const char *lines, uint32_t *n_lines)
{
    time_t ts = sched_utc(h->mono_us);
    char out[1024];
    size_t o = 0;
    const char *p = lines, *end = lines + h->len;
    while (p < end) {
        const char *nl = memchr(p, '\n', end - p);
        if (!nl) {
            break;
        }
        int r = snprintf(out + o, sizeof(out) - o, "%.*s %lld\n", (int)(nl - p), p, (long long)ts);
        if (r < 0 || o + r >= sizeof(out)) {
            if (!send_all(sock, out, o)) {
                return false;
            }
            o = 0;
            continue;   // same line again into the empty buffer
        }
        o += r;
        (*n_lines)++;
        p = nl + 1;
    }
    return send_all(sock, out, o);
}

static void flush(const appcfg_t *c)
{
    if (!c->carbon_host[0] || !sched_clock_valid() || s_len == 0 || esp_timer_get_time() < s_retry_at) {
        return;
    }
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM};
    char port[8];
    snprintf(port, sizeof(port), "%u", c->carbon_port);
    struct addrinfo *res = NULL;
    if (getaddrinfo(c->carbon_host, port, &hints, &res) != 0 || !res) {
        set_error("resolve", 0);
        ESP_LOGW(TAG, "event=resolve_failed host=%s", c->carbon_host);
        s_retry_at = esp_timer_get_time() + 5000000;
        return;
    }
    int sock = -1;
    int fam = c->prefer_v4 ? AF_INET : AF_INET6;   // preferred family first, then the other
    for (int pass = 0; pass < 2 && sock < 0; pass++) {
        for (struct addrinfo *a = res; a && sock < 0; a = a->ai_next) {
            if ((a->ai_family == fam) == (pass == 0)) {
                sock = connect_to(a);
                if (sock >= 0) {
                    char ip[46] = "";
                    if (a->ai_family == AF_INET) {
                        inet_ntop(AF_INET, &((struct sockaddr_in *)a->ai_addr)->sin_addr, ip, sizeof(ip));
                    } else {
                        inet_ntop(AF_INET6, &((struct sockaddr_in6 *)a->ai_addr)->sin6_addr, ip, sizeof(ip));
                    }
                    xSemaphoreTake(s_lock, portMAX_DELAY);
                    snprintf(s_st.peer, sizeof(s_st.peer), "%s", ip);
                    xSemaphoreGive(s_lock);
                }
            }
        }
    }
    freeaddrinfo(res);
    if (sock < 0) {
        int e = errno;
        set_error("connect", e);
        ESP_LOGW(TAG, "event=connect_failed host=%s port=%u err=\"%s\"", c->carbon_host, c->carbon_port, strerror(e));
        s_retry_at = esp_timer_get_time() + 5000000;
        return;
    }
    int64_t t0 = esp_timer_get_time();
    uint32_t batches = 0, lines = 0;
    for (;;) {
        // copy the oldest batch: building a new one may run meanwhile (no: same task) - but the
        // status readers take the lock, so keep the buffer consistent while it moves
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_len == 0) {
            xSemaphoreGive(s_lock);
            break;
        }
        batch_hdr_t h;
        memcpy(&h, s_buf, sizeof(h));
        xSemaphoreGive(s_lock);
        uint32_t n = 0;
        if (!send_batch(sock, &h, s_buf + sizeof(h), &n)) {
            int e = errno;
            set_error("send", e);
            ESP_LOGW(TAG, "event=send_failed err=\"%s\" sent_batches=%lu", strerror(e), (unsigned long)batches);
            s_retry_at = esp_timer_get_time() + 5000000;
            break;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        size_t first = sizeof(h) + h.len;
        memmove(s_buf, s_buf + first, s_len - first);
        s_len -= first;
        s_st.sent_batches++;
        s_st.sent_lines += n;
        s_st.last_ok_us = esp_timer_get_time();
        xSemaphoreGive(s_lock);
        batches++;
        lines += n;
    }
    close(sock);
    if (batches) {
        ESP_LOGD(TAG, "event=sent batches=%lu lines=%lu ms=%lld", (unsigned long)batches, (unsigned long)lines,
                 (esp_timer_get_time() - t0) / 1000);
    }
}

// --- task ---

static TickType_t ticks_until(int64_t t_us)
{
    int64_t us = t_us - esp_timer_get_time();
    if (us <= 0) {
        return 0;
    }
    int64_t tick_us = portTICK_PERIOD_MS * 1000LL;
    return (TickType_t)((us + tick_us - 1) / tick_us);
}

// The boundary after the last one handled - none is skipped. Only if the task fell behind by
// more than an interval, it continues with the latest boundary (the windows then span the gap).
static int64_t next_after(int iv, int64_t last)
{
    // + 0.5 s: an NTP correction can move the wall clock against esp_timer by milliseconds; the
    // boundary just handled must not come out as "the next one" again (intervals are >= 10 s)
    int64_t b = sched_next_us(iv, last + 500000);
    int64_t now = esp_timer_get_time();
    if (now - b > (int64_t)iv * 1000000) {
        b = sched_prev_us(iv, now);
    }
    return b;
}

static void check_late(const char *what, int64_t boundary, int64_t now)
{
    int64_t late = now - boundary - AFTER_US;
    if (late > 1000000) {
        ESP_LOGW(TAG, "event=late boundary=%s late_ms=%lld", what, late / 1000);
    }
}

static void carbon_task(void *arg)
{
    char watched[sizeof(((appcfg_t *)0)->carbon_host)] = "";
    health_id_t hid = health_register("carbon");
    for (;;) {
        health_alive(hid, (int64_t)sensors_interval_s() * 1000000);   // the wait below is at most that
        appcfg_t c;
        appcfg_get(&c);
        if (strcmp(watched, c.carbon_host) != 0) {
            strlcpy(watched, c.carbon_host, sizeof(watched));
            const char *names[1] = {c.carbon_host};
            netmgr_dns_watch("Carbon", names, c.carbon_host[0] ? 1 : 0);   // network page warns if it fails
        }
        int64_t nt = next_after(sensors_interval_s(), s_last_tick_us);
        int64_t nc = next_after(c.carbon_s, s_last_carbon_us);
        int64_t target = (nt < nc ? nt : nc) + AFTER_US;
        int64_t t0 = esp_timer_get_time();
        TickType_t ticks = ticks_until(target);
        ESP_LOGD(TAG, "event=wait now_ms=%lld tick_at_ms=%lld carbon_at_ms=%lld ticks=%lu", t0 / 1000, nt / 1000,
                 nc / 1000, (unsigned long)ticks);
        uint32_t notified = ulTaskNotifyTake(pdTRUE, ticks);
        int64_t now = esp_timer_get_time();
        ESP_LOGD(TAG, "event=woke now_ms=%lld notified=%lu", now / 1000, (unsigned long)notified);
        if (notified) {
            continue;   // reschedule: settings changed
        }
        if (now >= nt + AFTER_US) {
            check_late("sensor", nt, now);
            s_last_tick_us = nt;
            publish_self();
        }
        if (now >= nc + AFTER_US) {
            check_late("carbon", nc, now);
            s_last_carbon_us = nc;
            metrics_close_window(METRICS_WIN_CARBON);
            // The first window runs from boot (incl. the read cycle at boot) to the first
            // boundary: a partial interval with one reading too many - not sent.
            bool first = !s_first_window_done;
            s_first_window_done = true;
            if (c.carbon_host[0] && !first) {
                buffer_batch(nc, &c);
            }
        }
        flush(&c);
    }
}

esp_err_t carbon_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    // first windows start now: the first Carbon batch covers boot up to the first boundary
    s_last_tick_us = s_last_carbon_us = esp_timer_get_time();
    return xTaskCreate(carbon_task, "carbon", 6144, NULL, 3, &s_task) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
