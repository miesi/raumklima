#include "netlog.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "appcfg.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "health.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "netmgr.h"
#include "sched.h"


#define APP_NAME "raumklima-hub"
#define FACILITY 16                    // local0
#define RING_SLOTS 512                 // lines kept while the clock is not set or the host is down
#define TAG_LEN 24
#define MSG_LEN 216                    // longer messages are cut
#define RETRY_US (5LL * 1000 * 1000)
#define RESOLVE_EVERY_US (5LL * 60 * 1000 * 1000)
#define CONNECT_TIMEOUT_MS 3000

typedef struct {
    int64_t mono_us;
    uint8_t sev;
    char tag[TAG_LEN];
    char msg[MSG_LEN];
} rec_t;

// Static in PSRAM, not from the heap: kept out of the core dump (see carbon.c).
EXT_RAM_BSS_ATTR static rec_t s_ring[RING_SLOTS];
static uint32_t s_head, s_tail;        // free-running counters, slot = counter % RING_SLOTS
static SemaphoreHandle_t s_mutex;      // s_line, the ring and the status
static vprintf_like_t s_prev;
static char s_line[400];
static volatile esp_log_level_t s_console = ESP_LOG_INFO;
static volatile uint8_t s_sys_level = APPCFG_SEV_INFO;
static volatile bool s_enabled;
static TaskHandle_t s_task;
static netlog_status_t s_st;
// A line printed in pieces (the Wi-Fi library does that): the record that waits for the rest
static bool s_open;
static uint32_t s_open_idx;            // ring counter of the open record
static TaskHandle_t s_open_task;
#define OPEN_MAX_US (200LL * 1000)     // sent as is when the rest does not come

// ESP-IDF libraries that would flood the log at debug level; they stay at info
static const char *const k_noisy[] = {
    "wifi", "wifi_init", "phy_init", "pp", "net80211", "esp_netif_lwip", "esp_netif_handlers",
    "esp_netif_objects", "lwip", "event", "gpio", "spi_master", "i2c.master", "i2c.common", "nvs",
    "heap_init", "intr_alloc", "esp_timer", "clk", "spi_flash", "memspi", "esp_psram", "octal_psram",
    "cpu_start", "app_start", "system_api", "efuse", "esp_image", "coexist", "sleep", "pm", "rtc_clk",
    "uart", "dhcpc", "esp-tls", "task_wdt", "sntp", "esp_sntp", "esp_console", "esp_ringbuf", "vfs_calls",
};

static int sev_of(char c)
{
    switch (c) {
    case 'E': return APPCFG_SEV_ERROR;
    case 'W': return APPCFG_SEV_WARNING;
    case 'I': return APPCFG_SEV_INFO;
    case 'D':
    case 'V': return APPCFG_SEV_DEBUG;
    default: return -1;
    }
}

static esp_log_level_t level_of_sev(int sev)
{
    switch (sev) {
    case APPCFG_SEV_ERROR: return ESP_LOG_ERROR;
    case APPCFG_SEV_WARNING: return ESP_LOG_WARN;
    case APPCFG_SEV_DEBUG: return ESP_LOG_DEBUG;
    default: return ESP_LOG_INFO;
    }
}

// under s_mutex
static void push(int sev, const char *tag, size_t tag_len, const char *msg, size_t msg_len)
{
    if (s_head - s_tail >= RING_SLOTS) {
        s_tail++;   // drop the oldest
        if (s_enabled) {
            s_st.dropped++;
        }
    }
    rec_t *r = &s_ring[s_head % RING_SLOTS];
    r->mono_us = esp_timer_get_time();
    r->sev = sev;
    if (tag_len >= sizeof(r->tag)) {
        tag_len = sizeof(r->tag) - 1;
    }
    memcpy(r->tag, tag, tag_len);
    r->tag[tag_len] = '\0';
    if (msg_len >= sizeof(r->msg)) {
        msg_len = sizeof(r->msg) - 1;
    }
    memcpy(r->msg, msg, msg_len);
    r->msg[msg_len] = '\0';
    s_head++;
}

// The ESP_LOG output: "<color>W (1234) tag: message<color>\n" (log format v1). Printed on the
// console if its level passes the console threshold, buffered for syslog if it passes the
// syslog threshold.
static int hook(const char *fmt, va_list ap)
{
    if (xPortInIsrContext() || !s_mutex || xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) {
        return s_prev(fmt, ap);
    }
    va_list ap2;
    va_copy(ap2, ap);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int n = vsnprintf(s_line, sizeof(s_line), fmt, ap2);
    va_end(ap2);
    const char *p = s_line;
    while (*p == '\033') {   // color escape
        const char *m = strchr(p, 'm');
        if (!m) {
            break;
        }
        p = m + 1;
    }
    int sev = sev_of(p[0]);
    const char *tag = NULL, *msg = NULL;
    size_t tag_len = 0, msg_len = 0;
    bool complete = n > 0 && s_line[(size_t)n < sizeof(s_line) ? n - 1 : sizeof(s_line) - 2] == '\n';
    if (sev >= 0 && p[1] == ' ' && p[2] == '(') {
        const char *q = strchr(p, ')');
        if (q && q[1] == ' ') {
            tag = q + 2;
            const char *colon = strchr(tag, ':');
            if (colon) {
                tag_len = colon - tag;
                msg = colon + 1;
                if (*msg == ' ') {
                    msg++;
                }
                msg_len = strlen(msg);
                while (msg_len && (msg[msg_len - 1] == '\n' || msg[msg_len - 1] == '\r')) {
                    msg_len--;
                }
                if (msg_len >= 4 && memcmp(msg + msg_len - 4, "\033[0m", 4) == 0) {
                    msg_len -= 4;
                }
            }
        }
    }
    int r = n;
    if (!msg || level_of_sev(sev) <= s_console) {
        if (n >= 0 && (size_t)n < sizeof(s_line)) {
            fputs(s_line, stdout);
        } else {
            r = s_prev(fmt, ap);   // too long for the buffer: print it unshortened
        }
    }
    bool queued = false;
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    if (msg && sev <= s_sys_level) {
        push(sev, tag, tag_len, msg, msg_len);
        queued = s_enabled;
        s_open = !complete;
        s_open_idx = s_head - 1;
        s_open_task = me;
    } else if (!msg && s_open && s_open_task == me && s_open_idx - s_tail < RING_SLOTS) {
        // the rest of an open line: append it (without the newline)
        rec_t *r = &s_ring[s_open_idx % RING_SLOTS];
        size_t have = strlen(r->msg), add = strcspn(p, "\r\n");
        if (have + add >= sizeof(r->msg)) {
            add = sizeof(r->msg) - 1 - have;
        }
        memcpy(r->msg + have, p, add);
        r->msg[have + add] = '\0';
        s_open = !complete;
    } else if (msg) {
        s_open = false;
    }
    xSemaphoreGive(s_mutex);
    if (queued && s_task && xTaskGetCurrentTaskHandle() != s_task) {
        xTaskNotifyGive(s_task);
    }
    return r;
}

esp_err_t netlog_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    s_prev = esp_log_set_vprintf(hook);
    return ESP_OK;
}

void netlog_apply(void)
{
    appcfg_t c;
    appcfg_get(&c);
    s_sys_level = c.syslog_level;
    s_enabled = c.syslog_host[0] != '\0';
    esp_log_level_t sys = level_of_sev(c.syslog_level);
    esp_log_level_t con = s_console;
    // what is not printed and not sent is not formatted either
    esp_log_level_t base = sys < ESP_LOG_INFO ? sys : ESP_LOG_INFO;
    esp_log_level_set("*", base > con ? base : con);
    if (sys >= ESP_LOG_DEBUG) {
        esp_log_level_set("*", ESP_LOG_DEBUG);
    }
    for (size_t i = 0; i < sizeof(k_noisy) / sizeof(k_noisy[0]); i++) {
        esp_log_level_set(k_noisy[i], base > con ? base : con);
    }
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

void netlog_set_console_level(esp_log_level_t level)
{
    s_console = level;
    netlog_apply();
}

void netlog_get_status(netlog_status_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_st;
    out->buffered = s_head - s_tail;
    xSemaphoreGive(s_mutex);
    out->enabled = s_enabled;
    out->waiting_clock = s_enabled && !sched_clock_valid();
}

static void set_error(const char *what, int err)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_st.errors++;
    snprintf(s_st.last_error, sizeof(s_st.last_error), "%s: %s", what, err ? strerror(err) : "failed");
    xSemaphoreGive(s_mutex);
}

// "<134>1 2026-11-18T09:12:03.123Z host app - msgid - msgid: key=value ..."
static int format_line(const rec_t *r, const char *host, char *out, size_t len)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    int64_t wall = (int64_t)tv.tv_sec * 1000000 + tv.tv_usec + (r->mono_us - esp_timer_get_time());
    time_t sec = wall / 1000000;
    struct tm tm;
    gmtime_r(&sec, &tm);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", &tm);
    // MSGID: printable US-ASCII without space, max 32 (RFC 5424 6.2.7)
    char msgid[33];
    size_t k = 0;
    for (const char *t = r->tag; *t && k < sizeof(msgid) - 1; t++) {
        msgid[k++] = (*t > 32 && *t < 127) ? *t : '_';
    }
    msgid[k] = '\0';
    if (!k) {
        strlcpy(msgid, "-", sizeof(msgid));
    }
    int o = snprintf(out, len, "<%d>1 %s.%03dZ %s %s - %s - %s: ", FACILITY * 8 + r->sev, ts,
                     (int)((wall / 1000) % 1000), host[0] ? host : "-", APP_NAME, msgid, msgid);
    if (o < 0 || (size_t)o >= len) {
        return -1;
    }
    // key=value already ("event=..."): as is; free text: msg="..." with " and \ escaped
    const char *m = r->msg;
    const char *e = m;
    while (*e && (islower((unsigned char)*e) || isdigit((unsigned char)*e) || *e == '_')) {
        e++;
    }
    if (e > m && *e == '=') {
        o += snprintf(out + o, len - o, "%s", m);
    } else {
        o += snprintf(out + o, len - o, "msg=\"");
        for (; *m && (size_t)o < len - 3; m++) {
            if (*m == '"' || *m == '\\') {
                out[o++] = '\\';
            }
            out[o++] = (*m >= 32 && *m < 127) ? *m : '?';
        }
        out[o++] = '"';
        out[o] = '\0';
    }
    return (size_t)o < len ? o : (int)len - 1;
}

static int connect_tcp(const struct addrinfo *ai)
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
        struct timeval to = {.tv_sec = CONNECT_TIMEOUT_MS / 1000, .tv_usec = (CONNECT_TIMEOUT_MS % 1000) * 1000};
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

static void sender_task(void *arg)
{
    int sock = -1;
    struct addrinfo *ai = NULL;          // resolved target, preferred family first
    char cur[sizeof(((appcfg_t *)0)->syslog_host) + 16] = "";
    int64_t resolved_us = 0, retry_at = 0;
    static char line[512];
    health_id_t hid = health_register("netlog");
    for (;;) {
        health_alive(hid, 1000000);
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        appcfg_t c;
        appcfg_get(&c);
        char want[sizeof(cur)];
        snprintf(want, sizeof(want), "%s/%u/%c/%c", c.syslog_host, c.syslog_port, c.syslog_tcp ? 't' : 'u',
                 c.prefer_v4 ? '4' : '6');
        int64_t now = esp_timer_get_time();
        if (strcmp(want, cur) != 0 || (ai && now - resolved_us > RESOLVE_EVERY_US)) {
            if (sock >= 0) {
                close(sock);
                sock = -1;
            }
            if (ai) {
                freeaddrinfo(ai);
                ai = NULL;
            }
            strlcpy(cur, want, sizeof(cur));
            retry_at = 0;
            const char *names[1] = {c.syslog_host};
            netmgr_dns_watch("Syslog", names, c.syslog_host[0] ? 1 : 0);   // network page warns if it fails
        }
        if (!c.syslog_host[0] || !sched_clock_valid() || now < retry_at || s_head == s_tail) {
            continue;
        }
        if (!ai) {
            struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = c.syslog_tcp ? SOCK_STREAM : SOCK_DGRAM};
            char port[8];
            snprintf(port, sizeof(port), "%u", c.syslog_port);
            struct addrinfo *res = NULL;
            int e = getaddrinfo(c.syslog_host, port, &hints, &res);
            if (e != 0 || !res) {
                set_error("resolve", 0);
                retry_at = now + RETRY_US;
                continue;
            }
            // preferred family first: move the first entry of that family to the front
            int fam = c.prefer_v4 ? AF_INET : AF_INET6;
            for (struct addrinfo **pp = &res; *pp; pp = &(*pp)->ai_next) {
                if ((*pp)->ai_family == fam && pp != &res) {
                    struct addrinfo *x = *pp;
                    *pp = x->ai_next;
                    x->ai_next = res;
                    res = x;
                    break;
                }
            }
            ai = res;
            resolved_us = now;
        }
        if (sock < 0) {
            for (struct addrinfo *a = ai; a && sock < 0; a = a->ai_next) {
                sock = c.syslog_tcp ? connect_tcp(a) : socket(a->ai_family, SOCK_DGRAM, IPPROTO_UDP);
                if (sock >= 0 && !c.syslog_tcp && connect(sock, a->ai_addr, a->ai_addrlen) < 0) {
                    close(sock);   // UDP connect only fixes the destination; fails without a route
                    sock = -1;
                }
            }
            if (sock < 0) {
                set_error(c.syslog_tcp ? "connect" : "socket", errno);
                retry_at = now + RETRY_US;
                continue;
            }
        }
        for (;;) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            if (s_head == s_tail) {
                xSemaphoreGive(s_mutex);
                break;
            }
            uint32_t t = s_tail;
            rec_t r = s_ring[t % RING_SLOTS];
            bool wait = s_open && s_open_idx == t && esp_timer_get_time() - r.mono_us < OPEN_MAX_US;
            xSemaphoreGive(s_mutex);
            if (wait) {
                xTaskNotifyGive(s_task);   // its rest is coming: look again in a moment
                vTaskDelay(pdMS_TO_TICKS(50));
                break;
            }
            char *body = line + 8;   // room for the TCP octet count in front
            int n = r.msg[0] ? format_line(&r, c.hostname, body, sizeof(line) - 8) : 0;   // empty: skip
            bool ok = true;
            if (n > 0) {
                const char *p = body;
                int len = n;
                if (c.syslog_tcp) {
                    char pre[8];
                    int pl = snprintf(pre, sizeof(pre), "%d ", n);   // RFC 6587 octet counting
                    memcpy(body - pl, pre, pl);
                    p -= pl;
                    len += pl;
                }
                // a burst (the boot backlog) can run lwIP out of buffers: ENOMEM, wait and retry
                int r = -1;
                for (int i = 0; i < 20; i++) {
                    r = send(sock, p, len, 0);
                    if (r >= 0 || errno != ENOMEM) {
                        break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(20));
                }
                ok = r == len;
            }
            if (!ok) {
                set_error("send", errno);
                close(sock);
                sock = -1;
                retry_at = esp_timer_get_time() + RETRY_US;
                break;
            }
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            if (s_tail == t) {
                s_tail++;   // else the line was dropped meanwhile (ring full)
            }
            s_st.sent++;
            xSemaphoreGive(s_mutex);
            health_alive(hid, 1000000);   // a long backlog is progress, not a hang
        }
    }
}

esp_err_t netlog_start(void)
{
    netlog_apply();
    return xTaskCreate(sender_task, "netlog", 4096, NULL, 2, &s_task) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
