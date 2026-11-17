#include "sensors.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "app_events.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "i2cbus.h"
#include "metrics.h"
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

static sensor_inst_t s_inst[MAX_INST];
static int s_n;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_bus_lock;   // held for each driver poll; see sensors_bus_lock()
static int s_dropped;                 // sensors found beyond MAX_INST
static char s_dew_warn[96];           // condensation risk, from derive_dewpoint()
static char s_dew_logged[METRIC_KEY_LEN];   // sensor of the last logged warning, "" = none
static char s_dropped_addrs[40];

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
                    ESP_LOGW(TAG, "bus %d 0x%02x %s: init failed: %s", b + 1, addr, s->model, esp_err_to_name(err));
                    s->supported = false;
                }
                unique_name(s);
                ESP_LOGI(TAG, "bus %d 0x%02x: %s as '%s'%s", b + 1, addr, s->model, s->name,
                         s->supported ? "" : " (not supported)");
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
    if (!dot || strcmp(dot + 1, "temperature_c") != 0 || esp_timer_get_time() - m->mono_us > METRIC_STALE_US) {
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
    sensor_publish_as(inst, "dewpoint_c", dew);

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
            ESP_LOGW(TAG, "%s", w);
        } else {
            ESP_LOGI(TAG, "condensation risk over (dew point %.1f C)", dew);
        }
        strlcpy(s_dew_logged, who, sizeof(s_dew_logged));
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_dew_warn, w, sizeof(s_dew_warn));
    xSemaphoreGive(s_lock);
}

static void sensors_task(void *arg)
{
    int64_t next_dew = 0;
    for (;;) {
        if (esp_timer_get_time() >= next_dew) {
            derive_dewpoint();
            next_dew = esp_timer_get_time() + DEW_EVERY_US;
        }
        int64_t now = esp_timer_get_time();
        int64_t next = now + 1000000;
        for (int i = 0; i < s_n; i++) {
            sensor_inst_t *s = &s_inst[i];
            if (!s->supported) {
                continue;
            }
            if (s->next_us <= now) {
                xSemaphoreTake(s_bus_lock, portMAX_DELAY);
                int64_t due = s->drv->poll(s, now);
                xSemaphoreGive(s_bus_lock);
                xSemaphoreTake(s_lock, portMAX_DELAY);
                if (due < 0) {
                    s->err_count++;
                    s->next_us = esp_timer_get_time() + RETRY_US;
                } else {
                    s->ok_count++;
                    s->last_ok_us = esp_timer_get_time();
                    s->next_us = due;
                }
                xSemaphoreGive(s_lock);
                now = esp_timer_get_time();
            }
            if (s->next_us < next) {
                next = s->next_us;
            }
        }
        int64_t wait_us = next - esp_timer_get_time();
        if (wait_us > 0) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS((wait_us + 999) / 1000));   // sensors_kick() wakes early
        }
    }
}

esp_err_t sensors_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_bus_lock = xSemaphoreCreateMutex();
    probe_all();
    if (s_n == 0) {
        ESP_LOGW(TAG, "no sensors found");
    }
    if (s_dropped) {
        ESP_LOGE(TAG, "%d more sensor(s) found than supported (max %d) - ignored: %s", s_dropped, MAX_INST,
                 s_dropped_addrs);
    }
    esp_event_post(APP_EVENT, APP_EVENT_SENSORS_CHANGED, NULL, 0, 0);
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
