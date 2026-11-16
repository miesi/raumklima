// Bosch BME680 / BME688 with the BME68x SensorAPI (BSD-3-Clause).
// IAQ: BSEC2 or BSEC3 for the BME680 (the BME688 IAQ needs another config).
// Without BSEC support for the chip in this build: raw temperature, humidity, pressure, gas.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bme68x.h"
#include "bsec_glue.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sensor_driver.h"

static const char *TAG = "bme68x";

#define RAW_PERIOD_US (3LL * 1000 * 1000)

static const uint8_t k_addrs[] = {0x76, 0x77};

typedef struct {
    struct bme68x_dev dev;
    struct bme68x_conf conf;
    struct bme68x_heatr_conf heatr;
    bool use_bsec;
    bme_bsec_t bsec;
} ctx_t;

static BME68X_INTF_RET_TYPE api_read(uint8_t reg, uint8_t *data, uint32_t len, void *intf)
{
    return sensor_read_reg((sensor_inst_t *)intf, reg, data, len) == ESP_OK ? BME68X_OK : BME68X_E_COM_FAIL;
}

static BME68X_INTF_RET_TYPE api_write(uint8_t reg, const uint8_t *data, uint32_t len, void *intf)
{
    // the API writes register/value pairs: reg, d0, r1, d1 ... in one transaction
    return sensor_write_reg((sensor_inst_t *)intf, reg, data, len) == ESP_OK ? BME68X_OK : BME68X_E_COM_FAIL;
}

static void api_delay(uint32_t us, void *intf)
{
    if (us >= 2000) {
        vTaskDelay(pdMS_TO_TICKS((us + 999) / 1000));
    } else {
        esp_rom_delay_us(us);
    }
}

// one forced measurement with c->conf / c->heatr
static esp_err_t forced(ctx_t *c, bme_sample_t *out)
{
    int8_t r = bme68x_set_conf(&c->conf, &c->dev);
    if (r == BME68X_OK) {
        r = bme68x_set_heatr_conf(BME68X_FORCED_MODE, &c->heatr, &c->dev);
    }
    if (r == BME68X_OK) {
        r = bme68x_set_op_mode(BME68X_FORCED_MODE, &c->dev);
    }
    if (r != BME68X_OK) {
        return ESP_FAIL;
    }
    uint32_t dur_us = bme68x_get_meas_dur(BME68X_FORCED_MODE, &c->conf, &c->dev) + c->heatr.heatr_dur * 1000;
    api_delay(dur_us, c->dev.intf_ptr);
    struct bme68x_data d;
    uint8_t n = 0;
    if (bme68x_get_data(BME68X_FORCED_MODE, &d, &n, &c->dev) != BME68X_OK) {
        return ESP_FAIL;
    }
    out->new_data = n > 0 && (d.status & BME68X_NEW_DATA_MSK);
    out->temperature = d.temperature;
    out->pressure = d.pressure;
    out->humidity = d.humidity;
    out->gas_resistance = d.gas_resistance;
    out->gas_valid = (d.status & BME68X_GASM_VALID_MSK) && (d.status & BME68X_HEAT_STAB_MSK);
    return ESP_OK;
}

// BSEC asks for a measurement with its oversampling and heater settings
static esp_err_t bsec_measure(void *chip, const bsec_bme_settings_t *st, bme_sample_t *out)
{
    ctx_t *c = chip;
    c->conf.os_hum = st->humidity_oversampling;
    c->conf.os_temp = st->temperature_oversampling;
    c->conf.os_pres = st->pressure_oversampling;
    c->conf.filter = BME68X_FILTER_OFF;
    c->conf.odr = BME68X_ODR_NONE;
    c->heatr.enable = st->run_gas ? BME68X_ENABLE : BME68X_DISABLE;
    c->heatr.heatr_temp = st->heater_temperature;
    c->heatr.heatr_dur = st->heater_duration;
    return forced(c, out);
}

static esp_err_t probe(sensor_inst_t *s)
{
    uint8_t id = 0, variant = 0xff;
    if (sensor_read_reg(s, 0xD0, &id, 1) != ESP_OK || id != 0x61 || sensor_read_reg(s, 0xF0, &variant, 1) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    // chip ID 0x61 for BME680/688/690; variant ID 0xF0: 0x00 BME680, 0x01 BME688, 0x02 BME690
    // (bme68x_defs.h / bme69x_defs.h). The BME690 is handled by drivers/bme69x.c.
    if (variant == 0x00) {
        strlcpy(s->model, "BME680", sizeof(s->model));
        strlcpy(s->name, "bme680", sizeof(s->name));
    } else if (variant == 0x01) {
        strlcpy(s->model, "BME688", sizeof(s->model));
        strlcpy(s->name, "bme688", sizeof(s->name));
    } else {
        return ESP_ERR_NOT_FOUND;
    }
    ctx_t *c = calloc(1, sizeof(ctx_t));
    if (!c) {
        return ESP_ERR_NO_MEM;
    }
    s->ctx = c;
    c->dev.intf = BME68X_I2C_INTF;
    c->dev.intf_ptr = s;
    c->dev.read = api_read;
    c->dev.write = api_write;
    c->dev.delay_us = api_delay;
    c->dev.amb_temp = 22;
    int8_t r = bme68x_init(&c->dev);
    if (r != BME68X_OK) {
        ESP_LOGE(TAG, "bme68x_init: %d", r);
        return ESP_FAIL;
    }
    const char *why = "no BSEC config for this variant";
    c->use_bsec = variant == 0x00 && bme_bsec_init(&c->bsec, BSEC_CHIP_BME680, bsec_measure, c, &why);
    if (!c->use_bsec) {
        // Bosch example values for a single forced measurement
        c->conf.os_hum = BME68X_OS_2X;
        c->conf.os_temp = BME68X_OS_8X;
        c->conf.os_pres = BME68X_OS_4X;
        c->conf.filter = BME68X_FILTER_OFF;
        c->conf.odr = BME68X_ODR_NONE;
        c->heatr.enable = BME68X_ENABLE;
        c->heatr.heatr_temp = 320;
        c->heatr.heatr_dur = 150;
        snprintf(s->info, sizeof(s->info), "raw, no IAQ: %s", why);
        ESP_LOGW(TAG, "%s without IAQ: %s", s->model, why);
    }
    s->supported = true;
    return ESP_OK;
}

static int64_t poll(sensor_inst_t *s, int64_t now_us)
{
    ctx_t *c = s->ctx;
    if (c->use_bsec) {
        return bme_bsec_poll(s, &c->bsec, now_us);
    }
    bme_sample_t d = {0};
    if (forced(c, &d) != ESP_OK || !d.new_data) {
        return -1;
    }
    sensor_publish(s, "temperature_c", d.temperature);
    sensor_publish(s, "humidity_pct", d.humidity);
    sensor_publish(s, "pressure_hpa", d.pressure / 100.0f);
    if (d.gas_valid) {
        sensor_publish(s, "gas_ohm", d.gas_resistance);
    }
    return now_us + RAW_PERIOD_US;
}

static void save_state(sensor_inst_t *s)
{
    ctx_t *c = s->ctx;
    if (c && c->use_bsec) {
        bme_bsec_save(&c->bsec);
    }
}

const sensor_driver_t sensor_drv_bme68x = {
    .driver = "bme68x",
    .addrs = k_addrs,
    .n_addrs = sizeof(k_addrs),
    .probe = probe,
    .poll = poll,
    .save_state = save_state,
};
