// Bosch BME690 with the BME690 SensorAPI (BSD-3-Clause): temperature, humidity, pressure,
// gas resistance. No IAQ: BSEC2 does not support the BME690 and BSEC3 is only available as
// a click-through download from Bosch (not fetched automatically).
#include <stdlib.h>
#include <string.h>

#include "bme69x.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sensor_driver.h"

static const char *TAG = "bme69x";

#define PERIOD_US       (3LL * 1000 * 1000)
#define VARIANT_BME690  0x02    // register 0xF0, BME69X_REG_VARIANT_ID / BME690_VARIANT_GAS_HIGH

static const uint8_t k_addrs[] = {0x76, 0x77};

typedef struct {
    struct bme69x_dev dev;
    struct bme69x_conf conf;
    struct bme69x_heatr_conf heatr;
} ctx_t;

static BME69X_INTF_RET_TYPE api_read(uint8_t reg, uint8_t *data, uint32_t len, void *intf)
{
    return sensor_read_reg((sensor_inst_t *)intf, reg, data, len) == ESP_OK ? BME69X_OK : BME69X_E_COM_FAIL;
}

static BME69X_INTF_RET_TYPE api_write(uint8_t reg, const uint8_t *data, uint32_t len, void *intf)
{
    return sensor_write_reg((sensor_inst_t *)intf, reg, data, len) == ESP_OK ? BME69X_OK : BME69X_E_COM_FAIL;
}

static void api_delay(uint32_t us, void *intf)
{
    if (us >= 2000) {
        vTaskDelay(pdMS_TO_TICKS((us + 999) / 1000));
    } else {
        esp_rom_delay_us(us);
    }
}

static esp_err_t probe(sensor_inst_t *s)
{
    uint8_t id = 0, variant = 0xff;
    if (sensor_read_reg(s, 0xD0, &id, 1) != ESP_OK || id != 0x61 || sensor_read_reg(s, 0xF0, &variant, 1) != ESP_OK ||
        variant != VARIANT_BME690) {
        return ESP_ERR_NOT_FOUND;
    }
    strlcpy(s->model, "BME690", sizeof(s->model));
    strlcpy(s->name, "bme690", sizeof(s->name));

    ctx_t *c = calloc(1, sizeof(ctx_t));
    if (!c) {
        return ESP_ERR_NO_MEM;
    }
    s->ctx = c;
    c->dev.intf = BME69X_I2C_INTF;
    c->dev.intf_ptr = s;
    c->dev.read = api_read;
    c->dev.write = api_write;
    c->dev.delay_us = api_delay;
    c->dev.amb_temp = 22;
    int8_t r = bme69x_init(&c->dev);
    if (r != BME69X_OK) {
        ESP_LOGE(TAG, "bme69x_init: %d", r);
        return ESP_FAIL;
    }
    c->conf.os_hum = BME69X_OS_2X;
    c->conf.os_temp = BME69X_OS_8X;
    c->conf.os_pres = BME69X_OS_4X;
    c->conf.filter = BME69X_FILTER_OFF;
    c->conf.odr = BME69X_ODR_NONE;
    c->heatr.enable = BME69X_ENABLE;
    c->heatr.heatr_temp = 320;     // Bosch example values for a single forced measurement
    c->heatr.heatr_dur = 150;
    strlcpy(s->info, "raw values, no IAQ (needs BSEC3)", sizeof(s->info));
    s->supported = true;
    return ESP_OK;
}

static int64_t poll(sensor_inst_t *s, int64_t now_us)
{
    ctx_t *c = s->ctx;
    int8_t r = bme69x_set_conf(&c->conf, &c->dev);
    if (r == BME69X_OK) {
        r = bme69x_set_heatr_conf(BME69X_FORCED_MODE, &c->heatr, &c->dev);
    }
    if (r == BME69X_OK) {
        r = bme69x_set_op_mode(BME69X_FORCED_MODE, &c->dev);
    }
    if (r != BME69X_OK) {
        return -1;
    }
    uint32_t dur_us = bme69x_get_meas_dur(BME69X_FORCED_MODE, &c->conf, &c->dev) + c->heatr.heatr_dur * 1000;
    api_delay(dur_us, s);
    struct bme69x_data d;
    uint8_t n = 0;
    if (bme69x_get_data(BME69X_FORCED_MODE, &d, &n, &c->dev) != BME69X_OK || n == 0) {
        return -1;
    }
    sensor_publish(s, "temperature_c", d.temperature);
    sensor_publish(s, "humidity_pct", d.humidity);
    sensor_publish(s, "pressure_hpa", d.pressure / 100.0f);
    if ((d.status & BME69X_GASM_VALID_MSK) && (d.status & BME69X_HEAT_STAB_MSK)) {
        sensor_publish(s, "gas_ohm", d.gas_resistance);
    }
    return now_us + PERIOD_US;
}

const sensor_driver_t sensor_drv_bme69x = {
    .driver = "bme69x",
    .addrs = k_addrs,
    .n_addrs = sizeof(k_addrs),
    .probe = probe,
    .poll = poll,
};
