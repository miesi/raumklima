#include "sensors.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "app_events.h"
#include "appcfg.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "health.h"
#include "i2cbus.h"
#include "metrics.h"
#include "sched.h"
#include "sensor_driver.h"

static const char *TAG = "sensors";

// Registry. Order matters only if two drivers share an address.
static const sensor_driver_t *const k_drivers[] = {
    &sensor_drv_bme68x,
    &sensor_drv_bme69x,
    &sensor_drv_sht4x,
    &sensor_drv_scd4x,
    &sensor_drv_bh1750,
    &sensor_drv_ds2484,
};

#define MAX_INST SENSORS_MAX
#define RETRY_US (5LL * 1000 * 1000)
#define LEAD_US (100LL * 1000)           // read cycles end this long before the boundary
#define FIRST_CYCLE_GUESS_MS 1000        // lead before the first cycle was measured
#define CYCLE_HIST 8                     // the lead is the longest of the last CYCLE_HIST cycles

static sensor_inst_t s_inst[MAX_INST];
static int s_n;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_bus_lock;   // held for each driver poll; see sensors_bus_lock()
static int s_dropped;                 // sensors found beyond MAX_INST
static char s_dew_warn[96];           // condensation risk, from derive_dewpoint()
static char s_dew_logged[METRIC_KEY_LEN];   // sensor of the last logged warning, "" = none
static char s_dropped_addrs[40];
// read cycle of the sensors without a cadence (all of them together)
static bool s_cycle_running;
static int64_t s_cycle_start_us;
static int64_t s_cycle_next_us;          // planned start of the next cycle, 0 = right away
static volatile bool s_replan;
static uint32_t s_cycle_ms[CYCLE_HIST];
static int s_cycle_i;
static bool s_first_cycle_done;          // the first one includes the 1-Wire search: not a measure

esp_err_t sensor_read_reg(sensor_inst_t *s, uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s->dev, &reg, 1, buf, len, 100);
}

esp_err_t sensor_write_reg(sensor_inst_t *s, uint8_t reg, const uint8_t *buf, size_t len)
{
    uint8_t tmp[33];
    if (len + 1 > sizeof(tmp)) {
        return ESP_ERR_INVALID_SIZE;
    }
    tmp[0] = reg;
    memcpy(&tmp[1], buf, len);
    return i2c_master_transmit(s->dev, tmp, len + 1, 100);
}

void sensor_publish(sensor_inst_t *s, const char *quantity, float value)
{
    sensor_publish_as(s->name, quantity, value);
}

void sensor_publish_state(sensor_inst_t *s, const char *quantity, float value)
{
    char key[METRIC_KEY_LEN];
    snprintf(key, sizeof(key), "%s.%s", s->name, quantity);
    metrics_publish_state(key, value);
}

void sensor_publish_as(const char *instance, const char *quantity, float value)
{
    char key[METRIC_KEY_LEN];
    snprintf(key, sizeof(key), "%s.%s", instance, quantity);
    metrics_publish(key, value);
}

uint8_t sensirion_crc8(const uint8_t *data, size_t len)
{
    // Sensirion: polynomial 0x31, init 0xFF (embedded-i2c-* sensirion_i2c.h)
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

void sensors_kick(sensor_inst_t *s)
{
    s->next_us = 0;
    s->kicked = true;   // without a cadence and outside a cycle: run after_cycle() now
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

// Instance names are metric prefixes and must be unique: a second sensor of the same kind
// (e.g. on the other bus) gets "_2", "_3", ...
static void unique_name(sensor_inst_t *s)
{
    char base[sizeof(s->name)];
    strlcpy(base, s->name, sizeof(base));
    for (unsigned k = 2; k <= 9; k++) {
        bool dup = false;
        for (int i = 0; i < s_n; i++) {
            dup |= strcmp(s_inst[i].name, s->name) == 0;
        }
        if (!dup) {
            return;
        }
        snprintf(s->name, sizeof(s->name), "%.12s_%c", base, (char)('0' + k));
    }
}

static void probe_all(void)
{
    for (int b = 0; b < i2cbus_count(); b++) {
        i2c_master_bus_handle_t bus = i2cbus_handle(b);
        uint8_t claimed[128] = {0};
        for (size_t d = 0; d < sizeof(k_drivers) / sizeof(k_drivers[0]); d++) {
            const sensor_driver_t *drv = k_drivers[d];
            for (int a = 0; a < drv->n_addrs; a++) {
                uint8_t addr = drv->addrs[a];
                if (claimed[addr] || i2cbus_probe(b, addr) != ESP_OK) {
                    continue;
                }
                if (s_n >= MAX_INST) {
                    // no room: report it, keep the sensors found first
                    claimed[addr] = 1;
                    s_dropped++;
                    size_t l = strlen(s_dropped_addrs);
                    snprintf(s_dropped_addrs + l, sizeof(s_dropped_addrs) - l, "%s%d:0x%02x", l ? " " : "", b + 1,
                             addr);
                    continue;
                }
                sensor_inst_t *s = &s_inst[s_n];
                memset(s, 0, sizeof(*s));
                s->drv = drv;
                s->addr = addr;
                s->bus = b;
                const i2c_device_config_t dc = {
                    .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                    .device_address = addr,
                    .scl_speed_hz = i2cbus_freq_hz(),
                };
                if (i2c_master_bus_add_device(bus, &dc, &s->dev) != ESP_OK) {
                    continue;
                }
                esp_err_t err = drv->probe(s);
                if (err == ESP_ERR_NOT_FOUND) {
                    i2c_master_bus_rm_device(s->dev);
                    continue;
                }
                claimed[addr] = 1;
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "event=init_failed bus=%d addr=0x%02x model=%s err=%s", b + 1, addr, s->model, esp_err_to_name(err));
                    s->supported = false;
                }
                unique_name(s);
                ESP_LOGI(TAG, "event=found bus=%d addr=0x%02x model=%s name=%s supported=%d", b + 1, addr, s->model,
                         s->name, s->supported);
                s->next_us = esp_timer_get_time();
                s_n++;
            }
        }
    }
}

// --- condensation risk ---
// Dew point of the room air from an SHT4x (the accurate humidity source), Magnus formula with
// the coefficients of Sonntag (1990) over water: a = 17.62, b = 243.12 C. Every measured
// temperature (the SHT4x itself, SCD4x, BME6xx, 1-Wire probes on cold spots) that is within
// DEW_MARGIN_C of that dew point means condensation is possible there.
#define DEW_MARGIN_C 1.0f   // ASSUMPTION: our safety margin
#define DEW_EVERY_US (10LL * 1000 * 1000)

typedef struct {
    float dew;
    char worst_key[METRIC_KEY_LEN];
    float worst_t;
    bool hit;
} dew_ctx_t;

static void dew_check(const metric_t *m, void *arg)
{
    dew_ctx_t *d = arg;
    const char *dot = strrchr(m->key, '.');
    if (!dot || strcmp(dot + 1, "temperature_c") != 0 || !metrics_is_fresh(m)) {
        return;
    }
    if (m->value - d->dew <= DEW_MARGIN_C && (!d->hit || m->value < d->worst_t)) {
        d->hit = true;
        d->worst_t = m->value;
        strlcpy(d->worst_key, m->key, sizeof(d->worst_key));
    }
}

static void derive_dewpoint(void)
{
    metric_t t, rh;
    static int64_t last_mono;   // the SHT4x reading used last: a dew point per reading, not per call
    char inst[16] = "";
    // the primary temperature is an SHT4x when one is present (metrics priority sht4 > ...)
    if (!metrics_get_primary("temperature_c", &t) || strncmp(t.key, "sht4", 4) != 0) {
        s_dew_warn[0] = '\0';
        return;
    }
    const char *dot = strchr(t.key, '.');
    snprintf(inst, sizeof(inst), "%.*s", (int)(dot - t.key), t.key);
    char key[METRIC_KEY_LEN];
    snprintf(key, sizeof(key), "%s.humidity_pct", inst);
    if (!metrics_get(key, &rh) || rh.value <= 0) {
        return;
    }
    const float a = 17.62f, b = 243.12f;
    float g = logf(rh.value / 100.0f) + a * t.value / (b + t.value);
    float dew = b * g / (a - g);
    if (t.mono_us != last_mono) {
        last_mono = t.mono_us;
        sensor_publish_as(inst, "dewpoint_c", dew);
    }

    dew_ctx_t d = {.dew = dew};
    metrics_foreach(dew_check, &d);
    char w[sizeof(s_dew_warn)] = "";
    if (d.hit) {
        const char *k = d.worst_key;
        const char *kd = strchr(k, '.');
        snprintf(w, sizeof(w), "condensation risk: %.*s %.1f C, dew point %.1f C", (int)(kd - k), k, d.worst_t, dew);
    }
    // log on start, end and change of sensor only - not on every 0.1 K
    const char *who = d.hit ? d.worst_key : "";
    if (strcmp(who, s_dew_logged) != 0) {
        if (d.hit) {
            ESP_LOGW(TAG, "event=condensation_risk sensor=%s temperature_c=%.1f dewpoint_c=%.1f", d.worst_key, d.worst_t,
                     dew);
        } else {
            ESP_LOGI(TAG, "event=condensation_risk_over dewpoint_c=%.1f", dew);
        }
        strlcpy(s_dew_logged, who, sizeof(s_dew_logged));
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_dew_warn, w, sizeof(s_dew_warn));
    xSemaphoreGive(s_lock);
}

// --- scheduling ---
// Sensors with a cadence (BSEC, SCD4x) poll themselves at their pace. All others are read
// together once per sensor interval, in one cycle that starts early enough to end LEAD_US
// before the boundary, where the display refreshes and Carbon sends.

int sensors_interval_s(void)
{
    appcfg_t c;
    appcfg_get(&c);
    return c.display_s < c.carbon_s ? c.display_s : c.carbon_s;
}

uint32_t sensors_read_ms(void)
{
    uint32_t m = 0;
    for (int i = 0; i < CYCLE_HIST; i++) {
        if (s_cycle_ms[i] > m) {
            m = s_cycle_ms[i];
        }
    }
    return m;
}

void sensors_reschedule(void)
{
    s_replan = true;
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

static void on_time_synced(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    sensors_reschedule();   // boundaries move from "since boot" to the UTC clock
}

static int64_t plan_next_cycle(int64_t now)
{
    int iv = sensors_interval_s();
    uint32_t ms = sensors_read_ms();
    int64_t lead = (ms ? ms : FIRST_CYCLE_GUESS_MS) * 1000LL + LEAD_US;
    // a value read once per interval must survive a failed cycle before it counts as stale
    int64_t stale = iv * 2500000LL;
    metrics_set_stale_us(stale > 30000000LL ? stale : 30000000LL);
    return sched_next_us(iv, now + lead) - lead;
}

static bool has_cadence(const sensor_inst_t *s)
{
    return s->drv->cadence_us > 0;
}

static void start_cycle(int64_t now)
{
    int n = 0;
    for (int i = 0; i < s_n; i++) {
        sensor_inst_t *s = &s_inst[i];
        if (s->supported && !has_cadence(s)) {
            s->cycle_busy = true;
            s->cycle_start_us = 0;
            s->next_us = now;
            n++;
        }
    }
    s_cycle_running = n > 0;
    s_cycle_start_us = now;
}

static void end_cycle(int64_t now)
{
    uint32_t ms = (uint32_t)((now - s_cycle_start_us) / 1000);
    if (s_first_cycle_done) {
        s_cycle_ms[s_cycle_i] = ms;
        s_cycle_i = (s_cycle_i + 1) % CYCLE_HIST;
    }
    s_first_cycle_done = true;
    s_cycle_running = false;
    ESP_LOGD(TAG, "event=cycle_done read_ms=%lu", (unsigned long)ms);
    for (int i = 0; i < s_n; i++) {
        sensor_inst_t *s = &s_inst[i];
        if (s->supported && !has_cadence(s) && s->drv->after_cycle) {
            xSemaphoreTake(s_bus_lock, portMAX_DELAY);
            s->drv->after_cycle(s);
            xSemaphoreGive(s_bus_lock);
        }
    }
}

// one poll of one instance, with bookkeeping and the structured log of failures
static void poll_one(sensor_inst_t *s, int64_t now)
{
    if (!has_cadence(s) && s->cycle_start_us == 0) {
        s->cycle_start_us = now;
    }
    s->last_err = ESP_FAIL;
    xSemaphoreTake(s_bus_lock, portMAX_DELAY);
    int64_t due = s->drv->poll(s, now);
    xSemaphoreGive(s_bus_lock);
    int64_t t = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool cycle_end = false;
    if (due < 0) {
        s->err_count++;
        s->fail_streak++;
        if (has_cadence(s)) {
            s->next_us = t + RETRY_US;
        } else {
            cycle_end = true;   // next try with the next cycle
        }
    } else {
        s->ok_count++;
        s->last_ok_us = t;
        if (has_cadence(s)) {
            s->next_us = due;
        } else if (due == SENSOR_DONE) {
            cycle_end = true;
        } else {
            s->next_us = due;   // next step of this cycle
        }
    }
    if (cycle_end) {
        s->cycle_busy = false;
        s->read_ms = (uint32_t)((t - s->cycle_start_us) / 1000);
        if (s->read_ms > s->read_ms_max) {
            s->read_ms_max = s->read_ms;
        }
    }
    uint32_t streak = s->fail_streak;
    if (due >= 0) {
        s->fail_streak = 0;
    }
    xSemaphoreGive(s_lock);
    // the instance name is the subsystem of the log line (syslog MSGID)
    if (due < 0) {
        ESP_LOGW(s->name, "event=read_failed model=%s bus=%d addr=0x%02x err=%s errors=%lu streak=%lu", s->model,
                 s->bus + 1, s->addr, esp_err_to_name(s->last_err), (unsigned long)s->err_count,
                 (unsigned long)streak);
    } else if (streak) {
        ESP_LOGI(s->name, "event=read_ok after_failures=%lu", (unsigned long)streak);
    }
    if (cycle_end && due >= 0) {
        sensor_publish(s, "read_ms", s->read_ms);
        ESP_LOGD(s->name, "event=read read_ms=%lu", (unsigned long)s->read_ms);
    }
}

static void sensors_task(void *arg)
{
    int64_t next_dew = 0;
    health_id_t hid = health_register("sensors");
    for (;;) {
        health_alive(hid, 1000000);   // the wait at the end is at most 1 s
        if (esp_timer_get_time() >= next_dew) {
            derive_dewpoint();
            next_dew = esp_timer_get_time() + DEW_EVERY_US;
        }
        int64_t now = esp_timer_get_time();
        if (s_replan && !s_cycle_running) {
            s_replan = false;
            s_cycle_next_us = plan_next_cycle(now);
        }
        if (!s_cycle_running && now >= s_cycle_next_us) {
            start_cycle(now);
            if (!s_cycle_running) {
                s_cycle_next_us = plan_next_cycle(now);   // no sensor without a cadence
            }
        }
        int64_t next = now + 1000000;
        for (int i = 0; i < s_n; i++) {
            sensor_inst_t *s = &s_inst[i];
            if (s->kicked && s->supported && !has_cadence(s) && !s->cycle_busy) {
                s->kicked = false;   // e.g. "onewire scan": the search, without waiting for the next cycle
                if (s->drv->after_cycle) {
                    xSemaphoreTake(s_bus_lock, portMAX_DELAY);
                    s->drv->after_cycle(s);
                    xSemaphoreGive(s_bus_lock);
                }
            }
            if (!s->supported || (!has_cadence(s) && !s->cycle_busy)) {
                continue;
            }
            if (s->next_us <= now) {
                poll_one(s, now);
                now = esp_timer_get_time();
            }
            if ((has_cadence(s) || s->cycle_busy) && s->next_us < next) {
                next = s->next_us;
            }
        }
        if (s_cycle_running) {
            bool busy = false;
            for (int i = 0; i < s_n; i++) {
                busy |= s_inst[i].cycle_busy;
            }
            if (!busy) {
                end_cycle(now);
                s_cycle_next_us = plan_next_cycle(esp_timer_get_time());
            }
        }
        if (!s_cycle_running && s_cycle_next_us < next) {
            next = s_cycle_next_us;
        }
        if (next_dew < next) {
            next = next_dew;
        }
        int64_t wait_us = next - esp_timer_get_time();
        if (wait_us > 0) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS((wait_us + 999) / 1000));   // sensors_kick() wakes early
        }
    }
}

void sensors_cadence_help(char *buf, size_t len)
{
    size_t o = 0;
    buf[0] = '\0';
    for (size_t d = 0; d < sizeof(k_drivers) / sizeof(k_drivers[0]) && o < len; d++) {
        const sensor_driver_t *drv = k_drivers[d];
        int r;
        if (drv->cadence_us) {
            r = snprintf(buf + o, len - o, "  %-8s every %lld s, fixed: %s\n", drv->driver, drv->cadence_us / 1000000,
                         drv->cadence_why ? drv->cadence_why : "");
        } else {
            r = snprintf(buf + o, len - o, "  %-8s once per sensor interval\n", drv->driver);
        }
        o += r > 0 ? (size_t)r : 0;
    }
}

esp_err_t sensors_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_bus_lock = xSemaphoreCreateMutex();
    if (health_safe_mode()) {
        // after repeated crashes: no bus traffic at all; the console and the APIs still work
        // (no sensors), so the core dump can be fetched
        ESP_LOGW(TAG, "event=sensors_off reason=safe_mode");
        return ESP_OK;
    }
    probe_all();
    if (s_n == 0) {
        ESP_LOGW(TAG, "event=no_sensors");
    }
    if (s_dropped) {
        ESP_LOGE(TAG, "event=too_many_sensors over=%d max=%d ignored=\"%s\"", s_dropped, MAX_INST,
                 s_dropped_addrs);
    }
    esp_event_post(APP_EVENT, APP_EVENT_SENSORS_CHANGED, NULL, 0, 0);
    esp_event_handler_register(APP_EVENT, APP_EVENT_TIME_SYNCED, on_time_synced, NULL);
    // BSEC needs a large stack (work buffers on the stack in its examples, plus float math)
    xTaskCreate(sensors_task, "sensors", 8192, NULL, 4, &s_task);
    return ESP_OK;
}

int sensors_list(sensor_info_t *out, int max)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_n < max ? s_n : max;
    for (int i = 0; i < n; i++) {
        const sensor_inst_t *s = &s_inst[i];
        strlcpy(out[i].name, s->name, sizeof(out[i].name));
        strlcpy(out[i].model, s->model, sizeof(out[i].model));
        strlcpy(out[i].info, s->info, sizeof(out[i].info));
        out[i].addr = s->addr;
        out[i].bus = s->bus;
        out[i].supported = s->supported;
        out[i].ok_count = s->ok_count;
        out[i].err_count = s->err_count;
        out[i].last_ok_us = s->last_ok_us;
        out[i].cadence_us = s->drv->cadence_us;
        out[i].read_ms = s->read_ms;
        out[i].read_ms_max = s->read_ms_max;
        out[i].last_err = s->fail_streak ? s->last_err : 0;
    }
    xSemaphoreGive(s_lock);
    return n;
}

void sensors_save_state(void)
{
    // Called from the console: hold the poll lock so save_state() never runs while the sensors
    // task is inside the same driver (BSEC is not reentrant).
    sensors_bus_lock();
    for (int i = 0; i < s_n; i++) {
        if (s_inst[i].supported && s_inst[i].drv->save_state) {
            s_inst[i].drv->save_state(&s_inst[i]);
        }
    }
    sensors_bus_unlock();
}

void sensors_bus_lock(void)
{
    if (s_bus_lock) {
        xSemaphoreTake(s_bus_lock, portMAX_DELAY);
    }
}

void sensors_bus_unlock(void)
{
    if (s_bus_lock) {
        xSemaphoreGive(s_bus_lock);
    }
}

bool sensors_detail(int idx, char *buf, size_t len)
{
    if (idx < 0 || idx >= s_n || !s_inst[idx].supported || !s_inst[idx].drv->detail) {
        return false;
    }
    s_inst[idx].drv->detail(&s_inst[idx], buf, len);
    return true;
}

void sensors_warnings(char *buf, size_t len)
{
    buf[0] = '\0';
    if (s_dropped) {
        snprintf(buf, len, "%d sensor(s) over limit %d: %s", s_dropped, MAX_INST, s_dropped_addrs);   // bus:addr
    }
    char key[METRIC_KEY_LEN];
    if (metrics_overflow(key, sizeof(key))) {
        size_t l = strlen(buf);
        snprintf(buf + l, len - l, "%smetrics full (%d), dropping %s", l ? "; " : "", METRICS_MAX, key);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_dew_warn[0]) {
        size_t l = strlen(buf);
        snprintf(buf + l, len - l, "%s%s", l ? "; " : "", s_dew_warn);
    }
    for (int i = 0; i < s_n; i++) {
        if (s_inst[i].warn[0]) {
            size_t l = strlen(buf);
            snprintf(buf + l, len - l, "%s%s", l ? "; " : "", s_inst[i].warn);
        }
    }
    xSemaphoreGive(s_lock);
}
