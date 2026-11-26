// ROHM BH1750FVI ambient light sensor. No ID register: identified by address only.
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sensor_driver.h"

#define CMD_POWER_ON   0x01
#define CMD_RESET      0x07
#define CMD_CONT_HRES  0x10   // continuous high resolution mode, 1 lx, 120 ms typ / 180 ms max

static const uint8_t k_addrs[] = {0x23, 0x5C};   // ADDR low / high

static esp_err_t cmd(sensor_inst_t *s, uint8_t c)
{
    return i2c_master_transmit(s->dev, &c, 1, 100);
}

static esp_err_t probe(sensor_inst_t *s)
{
    strlcpy(s->model, "BH1750", sizeof(s->model));
    strlcpy(s->name, s->addr == 0x23 ? "bh1750" : "bh1750b", sizeof(s->name));
    s->supported = true;
    esp_err_t err = cmd(s, CMD_POWER_ON);
    if (err == ESP_OK) {
        err = cmd(s, CMD_RESET);
    }
    if (err == ESP_OK) {
        err = cmd(s, CMD_CONT_HRES);
    }
    vTaskDelay(pdMS_TO_TICKS(180));   // first conversion
    return err;
}

static int64_t poll(sensor_inst_t *s, int64_t now_us)
{
    uint8_t b[2];
    esp_err_t err = i2c_master_receive(s->dev, b, 2, 100);
    if (err != ESP_OK) {
        // the chip may have lost power - re-arm continuous mode
        cmd(s, CMD_POWER_ON);
        cmd(s, CMD_CONT_HRES);
        return sensor_fail(s, err);
    }
    float lux = ((b[0] << 8) | b[1]) / 1.2f;   // datasheet: count / 1.2 at MTreg default
    sensor_publish(s, "illuminance_lx", lux);
    return SENSOR_DONE;   // continuous mode: the chip measures on its own, we read the latest
}

const sensor_driver_t sensor_drv_bh1750 = {
    .driver = "bh1750",
    .addrs = k_addrs,
    .n_addrs = sizeof(k_addrs),
    .probe = probe,
    .poll = poll,
};
