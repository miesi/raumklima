// Sensirion SCD4x (SCD40/41/42/43) CO2 sensor in periodic measurement mode (5 s).
// Commands, delays and conversions from Sensirion's embedded-i2c-scd4x (BSD-3-Clause).
// Pressure compensation: the primary pressure metric (BME6xx) is sent once a minute.
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "metrics.h"
#include "sensor_driver.h"

static const char *TAG = "scd4x";

#define CMD_START_PERIODIC   0x21B1
#define CMD_STOP_PERIODIC    0x3F86   // 500 ms
#define CMD_READ_MEASUREMENT 0xEC05   // 1 ms, 3 words
#define CMD_DATA_READY       0xE4B8   // 1 ms, 1 word, ready if bits 10..0 != 0
#define CMD_SET_PRESSURE     0xE000   // 1 ms, word = hPa
#define CMD_SERIAL           0x3682   // 1 ms, 3 words
#define CMD_VARIANT          0x202F   // 1 ms, 1 word, bits 15..12

#define PERIOD_US            (5LL * 1000 * 1000)
#define PRESSURE_EVERY_US    (60LL * 1000 * 1000)

static const uint8_t k_addrs[] = {0x62};

typedef struct {
    int64_t last_pressure_us;
    uint16_t pressure_hpa;
} ctx_t;

static ctx_t s_ctx = {.last_pressure_us = -PRESSURE_EVERY_US};   // one SCD4x (fixed address); first compensation right away

static esp_err_t send(sensor_inst_t *s, uint16_t cmd)
{
    uint8_t b[2] = {cmd >> 8, cmd & 0xff};
    return i2c_master_transmit(s->dev, b, 2, 50);
}

static esp_err_t send_word(sensor_inst_t *s, uint16_t cmd, uint16_t w)
{
    uint8_t b[5] = {cmd >> 8, cmd & 0xff, w >> 8, w & 0xff, 0};
    b[4] = sensirion_crc8(&b[2], 2);
    return i2c_master_transmit(s->dev, b, 5, 50);
}

static esp_err_t read_words(sensor_inst_t *s, uint16_t cmd, uint16_t *w, int n)
{
    uint8_t b[9];
    esp_err_t err = send(s, cmd);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(2));
    err = i2c_master_receive(s->dev, b, n * 3, 50);
    if (err != ESP_OK) {
        return err;
    }
    for (int i = 0; i < n; i++) {
        if (sensirion_crc8(&b[3 * i], 2) != b[3 * i + 2]) {
            return ESP_ERR_INVALID_CRC;
        }
        w[i] = (b[3 * i] << 8) | b[3 * i + 1];
    }
    return ESP_OK;
}

static esp_err_t probe(sensor_inst_t *s)
{
    // may still measure from before a soft reboot; the sensor ignores most commands then
    send(s, CMD_STOP_PERIODIC);
    vTaskDelay(pdMS_TO_TICKS(500));
    uint16_t sn[3], var;
    if (read_words(s, CMD_SERIAL, sn, 3) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    const char *model = "SCD4x";
    if (read_words(s, CMD_VARIANT, &var, 1) == ESP_OK) {
        switch (var & 0xF000) {
        case 0x0000: model = "SCD40"; break;
        case 0x1000: model = "SCD41"; break;
        case 0x2000: model = "SCD42"; break;
        case 0x5000: model = "SCD43"; break;
        default: break;
        }
    }
    strlcpy(s->model, model, sizeof(s->model));
    snprintf(s->name, sizeof(s->name), "%c%c%c%c%c", model[0] | 0x20, model[1] | 0x20, model[2] | 0x20, model[3],
             model[4] == 'x' ? 'x' : model[4]);   // "scd41"
    snprintf(s->info, sizeof(s->info), "serial %04x%04x%04x", sn[0], sn[1], sn[2]);
    s->ctx = &s_ctx;
    if (send(s, CMD_START_PERIODIC) != ESP_OK) {
        return ESP_FAIL;
    }
    s->supported = true;
    return ESP_OK;
}

static void compensate_pressure(sensor_inst_t *s, ctx_t *c, int64_t now_us)
{
    metric_t p;
    if (now_us - c->last_pressure_us < PRESSURE_EVERY_US || !metrics_get_primary("pressure_hpa", &p)) {
        return;
    }
    uint16_t hpa = (uint16_t)(p.value + 0.5f);
    if (send_word(s, CMD_SET_PRESSURE, hpa) == ESP_OK) {
        if (hpa != c->pressure_hpa) {
            ESP_LOGI(TAG, "ambient pressure compensation %u hPa", hpa);
        }
        c->pressure_hpa = hpa;
        c->last_pressure_us = now_us;
        sensor_publish(s, "pressure_comp_hpa", hpa);
    }
}

static int64_t poll(sensor_inst_t *s, int64_t now_us)
{
    ctx_t *c = s->ctx;
    uint16_t ready;
    if (read_words(s, CMD_DATA_READY, &ready, 1) != ESP_OK) {
        return -1;
    }
    if ((ready & 0x07FF) == 0) {
        return now_us + 500 * 1000;   // not yet, look again soon
    }
    uint16_t w[3];
    if (read_words(s, CMD_READ_MEASUREMENT, w, 3) != ESP_OK) {
        return -1;
    }
    sensor_publish(s, "co2_ppm", w[0]);
    sensor_publish(s, "temperature_c", -45.0f + 175.0f * w[1] / 65536.0f);   // ((21875 * t) >> 13) - 45000
    sensor_publish(s, "humidity_pct", 100.0f * w[2] / 65536.0f);              // (12500 * h) >> 13
    compensate_pressure(s, c, now_us);
    return now_us + PERIOD_US;
}

const sensor_driver_t sensor_drv_scd4x = {
    .driver = "scd4x",
    .addrs = k_addrs,
    .n_addrs = sizeof(k_addrs),
    .probe = probe,
    .poll = poll,
};
