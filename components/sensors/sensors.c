#include "sensors.h"

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
    &sensor_drv_bh1750,
    &sensor_drv_ds2484,
};

#define MAX_INST SENSORS_MAX
#define RETRY_US (5LL * 1000 * 1000)

static sensor_inst_t s_inst[MAX_INST];
static int s_n;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static int s_dropped;                 // sensors found beyond MAX_INST
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

void sensors_kick(sensor_inst_t *s)
{
    s->next_us = 0;
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

static void probe_all(void)
{
    i2c_master_bus_handle_t bus = i2cbus_handle();
    uint8_t claimed[128] = {0};
    for (size_t d = 0; d < sizeof(k_drivers) / sizeof(k_drivers[0]); d++) {
        const sensor_driver_t *drv = k_drivers[d];
        for (int a = 0; a < drv->n_addrs; a++) {
            uint8_t addr = drv->addrs[a];
            if (claimed[addr] || i2c_master_probe(bus, addr, 50) != ESP_OK) {
                continue;
            }
            if (s_n >= MAX_INST) {
                // no room: report it, keep the sensors found first
                claimed[addr] = 1;
                s_dropped++;
                size_t l = strlen(s_dropped_addrs);
                snprintf(s_dropped_addrs + l, sizeof(s_dropped_addrs) - l, "%s0x%02x", l ? " " : "", addr);
                continue;
            }
            sensor_inst_t *s = &s_inst[s_n];
            memset(s, 0, sizeof(*s));
            s->drv = drv;
            s->addr = addr;
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
                ESP_LOGW(TAG, "0x%02x %s: init failed: %s", addr, s->model, esp_err_to_name(err));
                s->supported = false;
            }
            ESP_LOGI(TAG, "0x%02x: %s as '%s'%s", addr, s->model, s->name, s->supported ? "" : " (not supported)");
            s->next_us = esp_timer_get_time();
            s_n++;
        }
    }
}

static void sensors_task(void *arg)
{
    for (;;) {
        int64_t now = esp_timer_get_time();
        int64_t next = now + 1000000;
        for (int i = 0; i < s_n; i++) {
            sensor_inst_t *s = &s_inst[i];
            if (!s->supported) {
                continue;
            }
            if (s->next_us <= now) {
                int64_t due = s->drv->poll(s, now);
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
    for (int i = 0; i < s_n; i++) {
        if (s_inst[i].supported && s_inst[i].drv->save_state) {
            s_inst[i].drv->save_state(&s_inst[i]);
        }
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
        snprintf(buf, len, "%d sensor(s) over limit %d", s_dropped, MAX_INST);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_n; i++) {
        if (s_inst[i].warn[0]) {
            size_t l = strlen(buf);
            snprintf(buf + l, len - l, "%s%s", l ? "; " : "", s_inst[i].warn);
        }
    }
    xSemaphoreGive(s_lock);
}
