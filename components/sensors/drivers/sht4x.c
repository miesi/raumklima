// Sensirion SHT4x (SHT40/41/43/45). Commands and conversions from Sensirion's
// embedded-i2c-sht4x (BSD-3-Clause). The variants share the command set and have no
// product ID command there, so the firmware reports "SHT4x".
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sensor_driver.h"

#define CMD_MEASURE_HIGH  0xFD   // high repeatability, 10 ms
#define CMD_SERIAL        0x89
#define PERIOD_US         (2LL * 1000 * 1000)

static const uint8_t k_addrs[] = {0x44, 0x45, 0x46};

static esp_err_t cmd_read(sensor_inst_t *s, uint8_t cmd, int wait_ms, uint8_t *buf)
{
    esp_err_t err = i2c_master_transmit(s->dev, &cmd, 1, 50);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(wait_ms));
    err = i2c_master_receive(s->dev, buf, 6, 50);
    if (err != ESP_OK) {
        return err;
    }
    // two words, each followed by its CRC
    if (sensirion_crc8(buf, 2) != buf[2] || sensirion_crc8(buf + 3, 2) != buf[5]) {
        return ESP_ERR_INVALID_CRC;
    }
    return ESP_OK;
}

static esp_err_t probe(sensor_inst_t *s)
{
    uint8_t b[6];
    if (cmd_read(s, CMD_SERIAL, 2, b) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;   // no SHT4x answer with valid CRC
    }
    uint32_t serial = ((uint32_t)b[0] << 24) | (b[1] << 16) | (b[3] << 8) | b[4];
    strlcpy(s->model, "SHT4x", sizeof(s->model));
    strlcpy(s->name, s->addr == 0x44 ? "sht4x" : (s->addr == 0x45 ? "sht4x_b" : "sht4x_c"), sizeof(s->name));
    snprintf(s->info, sizeof(s->info), "serial %08lx", (unsigned long)serial);
    s->supported = true;
    return ESP_OK;
}

static int64_t poll(sensor_inst_t *s, int64_t now_us)
{
    uint8_t b[6];
    if (cmd_read(s, CMD_MEASURE_HIGH, 10, b) != ESP_OK) {
        return -1;
    }
    uint16_t t = (b[0] << 8) | b[1];
    uint16_t h = (b[3] << 8) | b[4];
    float temp = -45.0f + 175.0f * t / 65536.0f;   // = ((21875 * t) >> 13) - 45000 m degC
    float rh = -6.0f + 125.0f * h / 65536.0f;      // = ((15625 * h) >> 13) - 6000 m %RH
    if (rh < 0) {
        rh = 0;
    } else if (rh > 100) {
        rh = 100;
    }
    sensor_publish(s, "temperature_c", temp);
    sensor_publish(s, "humidity_pct", rh);
    return now_us + PERIOD_US;
}

const sensor_driver_t sensor_drv_sht4x = {
    .driver = "sht4x",
    .addrs = k_addrs,
    .n_addrs = sizeof(k_addrs),
    .probe = probe,
    .poll = poll,
};
