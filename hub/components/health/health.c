#include "health.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app_events.h"
#include "bootloader_random.h"
#include "esp_attr.h"
#include "esp_core_dump.h"
#include "esp_event.h"
#include "esp_random.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "health";

#define RTC_MAGIC 0x484c5448u                 // "HTLH"
#define STALL_SLACK_US (30LL * 1000 * 1000)   // overdue limit = 2 x interval + this
#define SUPERVISOR_PERIOD_MS 1000

// Kept in RTC slow memory: survives panic and watchdog resets and esp_restart(), not a
// power-on. A check word catches garbage after power-on or a reset that cleared the RTC domain.
typedef struct {
    uint32_t magic;
    uint32_t boots;
    uint32_t crashes;
    uint32_t run_s;                           // uptime of the current run, updated every second
    uint32_t hist_n;
    uint32_t hist_s[HEALTH_SAFE_CRASHES];     // run times of the runs that ended in a crash, newest first
    char stall[16];                           // set by the supervisor right before it aborts
    uint32_t check;
} rtc_state_t;

static RTC_NOINIT_ATTR rtc_state_t s_rtc;

typedef struct {
    char name[16];
    int64_t last_us;      // 0 = not started
    int64_t allowed_us;
} watch_t;

static watch_t s_w[HEALTH_MAX_WATCHED];
static int s_nw;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;   // s_w (64-bit values) and s_rtc

static esp_reset_reason_t s_reason;
static bool s_safe;
static char s_last_stall[16];
static esp_err_t s_cd_err = ESP_ERR_NOT_FOUND;
static char s_token[HEALTH_TOKEN_LEN + 1];

// --- RTC state ---

static uint32_t rtc_check(void)
{
    const uint32_t *w = (const uint32_t *)&s_rtc;
    uint32_t h = 2166136261u;   // FNV-1a over the words before "check"
    for (size_t i = 0; i < offsetof(rtc_state_t, check) / sizeof(uint32_t); i++) {
        h = (h ^ w[i]) * 16777619u;
    }
    return h;
}

static void rtc_seal(void)
{
    s_rtc.check = rtc_check();
}

static bool is_crash(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
    case ESP_RST_CPU_LOCKUP:
        return true;
    default:
        return false;   // power-on, reboot command, brownout, deep sleep, ...
    }
}

static const char *reason_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON: return "power_on";
    case ESP_RST_EXT: return "external";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt_watchdog";
    case ESP_RST_TASK_WDT: return "task_watchdog";
    case ESP_RST_WDT: return "other_watchdog";
    case ESP_RST_DEEPSLEEP: return "deep_sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_USB: return "usb";
    case ESP_RST_JTAG: return "jtag";
    case ESP_RST_PWR_GLITCH: return "power_glitch";
    case ESP_RST_CPU_LOCKUP: return "cpu_lockup";
    default: return "unknown";
    }
}

// --- core dump ---

// Copies s into a double-quoted log value: no quotes, no control characters.
static void sanitize(char *s)
{
    for (; *s; s++) {
        if (*s == '"') {
            *s = '\'';
        } else if ((unsigned char)*s < ' ') {
            *s = ' ';
        }
    }
}

esp_err_t health_coredump_summary(char *buf, size_t len)
{
    if (s_cd_err != ESP_OK) {
        return s_cd_err;
    }
    esp_core_dump_summary_t *s = malloc(sizeof(*s));
    if (!s) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_core_dump_get_summary(s);
    if (err == ESP_OK) {
        char reason[80] = "";
        if (esp_core_dump_get_panic_reason(reason, sizeof(reason)) != ESP_OK) {
            strlcpy(reason, "-", sizeof(reason));
        }
        sanitize(reason);
        s->exc_task[sizeof(s->exc_task) - 1] = '\0';
        size_t addr = 0, size = 0;
        esp_core_dump_image_get(&addr, &size);
        // PC and backtrace only when the summary's backtrace is intact: after an abort or a
        // watchdog they come out as garbage (VERIFIED: pc=0xfffffffd, corrupted=1), the full
        // dump has the right ones (idf.py coredump-info)
        char pc[20] = "";
        if (!s->exc_bt_info.corrupted) {
            snprintf(pc, sizeof(pc), " pc=0x%08lx", (unsigned long)s->exc_pc);
        }
        snprintf(buf, len, "task=%s%s exc_cause=%lu exc_vaddr=0x%08lx size=%u reason=\"%s\"", s->exc_task, pc,
                 (unsigned long)s->ex_info.exc_cause, (unsigned long)s->ex_info.exc_vaddr, (unsigned)size, reason);
    }
    free(s);
    return err;
}

// Backtrace as bare hex words (addr2line takes them as they are), fits one syslog line.
static esp_err_t coredump_backtrace(char *buf, size_t len)
{
    esp_core_dump_summary_t *s = malloc(sizeof(*s));
    if (!s) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_core_dump_get_summary(s);
    if (err == ESP_OK && s->exc_bt_info.corrupted) {
        err = ESP_ERR_INVALID_STATE;   // nothing useful to log
    }
    if (err == ESP_OK) {
        size_t o = snprintf(buf, len, "depth=%lu bt=\"", (unsigned long)s->exc_bt_info.depth);
        uint32_t n = s->exc_bt_info.depth < 16 ? s->exc_bt_info.depth : 16;
        for (uint32_t i = 0; i < n && o < len; i++) {
            o += snprintf(buf + o, len - o, "%s%08lx", i ? " " : "", (unsigned long)s->exc_bt_info.bt[i]);
        }
        if (o < len) {
            snprintf(buf + o, len - o, "\"");
        }
    }
    free(s);
    return err;
}

esp_err_t health_coredump_erase(void)
{
    esp_err_t err = esp_core_dump_image_erase();
    if (err == ESP_OK) {
        s_cd_err = ESP_ERR_NOT_FOUND;
        ESP_LOGI(TAG, "event=coredump_erased");
        esp_event_post(APP_EVENT, APP_EVENT_COREDUMP_ERASED, NULL, 0, 0);
    } else {
        ESP_LOGE(TAG, "event=coredump_erase_failed err=%s", esp_err_to_name(err));
    }
    return err;
}

static void coredump_report(void)
{
    s_cd_err = esp_core_dump_image_check();
    if (s_cd_err == ESP_ERR_NOT_FOUND) {
        if (is_crash(s_reason) && s_reason != ESP_RST_WDT) {
            // the panic handler ran but left no dump: too large for the partition, or cut off
            // (after an interrupt watchdog panic, see sdkconfig.defaults)
            ESP_LOGW(TAG, "event=coredump_missing reason=%s hint=\"see the panic output on the console\"",
                     reason_name(s_reason));
        }
        return;
    }
    if (s_cd_err != ESP_OK) {
        // With "no overwrite" a damaged dump would block every later one: erase it.
        ESP_LOGW(TAG, "event=coredump_invalid err=%s action=erase", esp_err_to_name(s_cd_err));
        health_coredump_erase();
        return;
    }
    static char line[200];
    if (health_coredump_summary(line, sizeof(line)) == ESP_OK) {
        ESP_LOGW(TAG, "event=coredump_found %s", line);
    } else {
        ESP_LOGW(TAG, "event=coredump_found summary=unreadable");
    }
    if (coredump_backtrace(line, sizeof(line)) == ESP_OK) {
        ESP_LOGW(TAG, "event=coredump_backtrace %s", line);
    }
    ESP_LOGW(TAG, "event=coredump_http port=80 path=/coredump token=%s", s_token);
}

bool health_coredump_token(char *buf, size_t len)
{
    if (s_cd_err != ESP_OK) {
        return false;
    }
    strlcpy(buf, s_token, len);
    return true;
}

// Before the radio starts, esp_random() is not a true random source; the SAR ADC entropy source
// makes it one (bootloader_random.h). Called in health_init(), before board_init() and WLAN.
static void make_token(void)
{
    uint8_t r[HEALTH_TOKEN_LEN / 2];
    bootloader_random_enable();
    esp_fill_random(r, sizeof(r));
    bootloader_random_disable();
    for (size_t i = 0; i < sizeof(r); i++) {
        snprintf(&s_token[2 * i], 3, "%02x", r[i]);
    }
}

// --- init, safe mode ---

void health_init(void)
{
    s_reason = esp_reset_reason();
    bool valid = s_rtc.magic == RTC_MAGIC && s_rtc.check == rtc_check() && s_rtc.hist_n <= HEALTH_SAFE_CRASHES;
    if (!valid) {
        memset(&s_rtc, 0, sizeof(s_rtc));
        s_rtc.magic = RTC_MAGIC;
    }
    s_rtc.stall[sizeof(s_rtc.stall) - 1] = '\0';
    strlcpy(s_last_stall, s_rtc.stall, sizeof(s_last_stall));
    s_rtc.boots++;
    if (is_crash(s_reason)) {
        // run_s is 0 if the RTC state was lost: counts as a crash right after boot
        memmove(&s_rtc.hist_s[1], &s_rtc.hist_s[0], sizeof(s_rtc.hist_s[0]) * (HEALTH_SAFE_CRASHES - 1));
        s_rtc.hist_s[0] = s_rtc.run_s;
        if (s_rtc.hist_n < HEALTH_SAFE_CRASHES) {
            s_rtc.hist_n++;
        }
        s_rtc.crashes++;
    } else {
        s_rtc.hist_n = 0;
        s_last_stall[0] = '\0';
    }
    uint32_t sum = 0;
    for (uint32_t i = 0; i < s_rtc.hist_n; i++) {
        sum += s_rtc.hist_s[i];
    }
    s_safe = s_rtc.hist_n >= HEALTH_SAFE_CRASHES && sum < HEALTH_SAFE_WINDOW_S;
    s_rtc.run_s = 0;
    s_rtc.stall[0] = '\0';
    rtc_seal();

    char hist[40] = "";
    size_t o = 0;
    for (uint32_t i = 0; i < s_rtc.hist_n && o < sizeof(hist); i++) {
        o += snprintf(hist + o, sizeof(hist) - o, "%s%lu", i ? " " : "", (unsigned long)s_rtc.hist_s[i]);
    }
    if (is_crash(s_reason)) {
        ESP_LOGW(TAG, "event=crash_reset reason=%s stall=%s boots=%lu crashes=%lu crash_run_s=\"%s\"",
                 reason_name(s_reason), s_last_stall[0] ? s_last_stall : "-", (unsigned long)s_rtc.boots,
                 (unsigned long)s_rtc.crashes, hist);
    } else {
        ESP_LOGI(TAG, "event=reset reason=%s boots=%lu crashes=%lu", reason_name(s_reason),
                 (unsigned long)s_rtc.boots, (unsigned long)s_rtc.crashes);
    }
    if (s_safe) {
        ESP_LOGE(TAG, "event=safe_mode crashes=%lu run_s_total=%lu limit_s=%d sensors=off display=static leave=reboot",
                 (unsigned long)s_rtc.hist_n, (unsigned long)sum, HEALTH_SAFE_WINDOW_S);
    }
    make_token();
    coredump_report();
}

bool health_safe_mode(void)
{
    return s_safe;
}

// --- watch ---

health_id_t health_register(const char *name)
{
    health_id_t id = -1;
    taskENTER_CRITICAL(&s_mux);
    if (s_nw < HEALTH_MAX_WATCHED) {
        id = s_nw++;
        strlcpy(s_w[id].name, name, sizeof(s_w[id].name));
    }
    taskEXIT_CRITICAL(&s_mux);
    if (id < 0) {
        ESP_LOGE(TAG, "event=watch_table_full task=%s max=%d", name, HEALTH_MAX_WATCHED);
    }
    return id;
}

void health_alive(health_id_t id, int64_t interval_us)
{
    if (id < 0 || id >= HEALTH_MAX_WATCHED) {
        return;
    }
    int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_mux);
    s_w[id].allowed_us = 2 * interval_us + STALL_SLACK_US;
    s_w[id].last_us = now > 0 ? now : 1;
    taskEXIT_CRITICAL(&s_mux);
}

static void supervisor_task(void *arg)
{
    esp_task_wdt_add(NULL);   // a hung supervisor is caught by the task watchdog
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(SUPERVISOR_PERIOD_MS));
        esp_task_wdt_reset();
        int64_t now = esp_timer_get_time();
        int stalled = -1;
        int64_t age = 0, allowed = 0;
        taskENTER_CRITICAL(&s_mux);
        s_rtc.run_s = (uint32_t)(now / 1000000);
        for (int i = 0; i < s_nw && stalled < 0; i++) {
            if (s_w[i].last_us && now - s_w[i].last_us > s_w[i].allowed_us) {
                stalled = i;
                age = now - s_w[i].last_us;
                allowed = s_w[i].allowed_us;
                strlcpy(s_rtc.stall, s_w[i].name, sizeof(s_rtc.stall));
            }
        }
        rtc_seal();
        taskEXIT_CRITICAL(&s_mux);
        if (stalled >= 0) {
            ESP_LOGE(TAG, "event=stall task=%s age_s=%lld allowed_s=%lld action=abort", s_w[stalled].name,
                     age / 1000000, allowed / 1000000);
            char why[40];
            snprintf(why, sizeof(why), "stall: %s", s_w[stalled].name);
            esp_system_abort(why);   // panic handler: core dump with all task stacks, then reset
        }
    }
}

esp_err_t health_start(void)
{
    // above the application tasks (<= 5): it must run while one of them is stuck
    return xTaskCreate(supervisor_task, "health", 3072, NULL, 10, NULL) == pdPASS ? ESP_OK : ESP_FAIL;
}

void health_get_status(health_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->safe_mode = s_safe;
    out->reset_reason = s_reason;
    strlcpy(out->last_stall, s_last_stall, sizeof(out->last_stall));
    out->coredump = s_cd_err == ESP_OK;
    out->coredump_err = s_cd_err;
    int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_mux);
    out->boots = s_rtc.boots;
    out->crashes = s_rtc.crashes;
    out->history_n = (int)s_rtc.hist_n;
    memcpy(out->history_s, s_rtc.hist_s, sizeof(out->history_s));
    out->n_watch = s_nw;
    for (int i = 0; i < s_nw; i++) {
        strlcpy(out->watch[i].name, s_w[i].name, sizeof(out->watch[i].name));
        out->watch[i].started = s_w[i].last_us != 0;
        out->watch[i].age_us = s_w[i].last_us ? now - s_w[i].last_us : 0;
        out->watch[i].allowed_us = s_w[i].allowed_us;
    }
    taskEXIT_CRITICAL(&s_mux);
}

// --- crash tests ---

static void spin_task(void *arg)
{
    for (;;) {   // never yields: IDLE1 starves, task watchdog after its timeout
        __asm__ volatile("" ::: "memory");
    }
}

void health_crash(const char *how)
{
    ESP_LOGW(TAG, "event=crash_test how=%s", how);
    vTaskDelay(pdMS_TO_TICKS(100));   // let the line reach the console
    if (strcmp(how, "fault") == 0) {
        volatile uint32_t *p = NULL;
        *p = 1;   // store to address 0: exception, panic handler
    } else if (strcmp(how, "spin") == 0) {
        // Pinned: an unpinned task that spins at a low priority is moved between the cores
        // whenever something preempts it, both idle tasks still run now and then and the task
        // watchdog stays fed (VERIFIED: a spin in the console task, priority 2, no affinity,
        // ran for minutes without a timeout). The supervisor catches that case for the watched
        // tasks: their loop stops reporting.
        xTaskCreatePinnedToCore(spin_task, "spin", 2048, NULL, 1, NULL, 1);
        return;
    } else if (strcmp(how, "irqoff") == 0) {
        portDISABLE_INTERRUPTS();   // no tick on this core: interrupt watchdog
        for (;;) {
            __asm__ volatile("" ::: "memory");
        }
    }
    esp_system_abort("crash test: panic");
}
