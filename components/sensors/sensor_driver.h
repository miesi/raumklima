// Interface every sensor driver implements. Adding a sensor = new file in drivers/ plus
// one line in the registry in sensors.c.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

typedef struct sensor_inst sensor_inst_t;

typedef struct {
    const char *driver;            // e.g. "bme68x"
    const uint8_t *addrs;          // candidate I2C addresses
    uint8_t n_addrs;
    // Identify the chip at s->addr (s->dev is ready). Fill s->name, s->model, s->supported,
    // s->ctx. Return ESP_ERR_NOT_FOUND if this is not our chip.
    esp_err_t (*probe)(sensor_inst_t *s);
    // Do one step of work, publish metrics. Return the esp_timer time (us) of the next call.
    // On error: return a negative value; the registry counts it and retries in 5 s.
    int64_t (*poll)(sensor_inst_t *s, int64_t now_us);
    void (*save_state)(sensor_inst_t *s);    // optional
    // optional: multi-line text for the display/console detail page (e.g. one line per 1-Wire probe)
    void (*detail)(sensor_inst_t *s, char *buf, size_t len);
} sensor_driver_t;

struct sensor_inst {
    const sensor_driver_t *drv;
    i2c_master_dev_handle_t dev;
    uint8_t addr;
    char name[16];
    char model[24];
    char info[48];
    char warn[80];         // non-empty = problem to show on the overview
    bool supported;
    void *ctx;
    int64_t next_us;
    uint32_t ok_count;
    uint32_t err_count;
    int64_t last_ok_us;
};

// helpers for drivers
esp_err_t sensor_read_reg(sensor_inst_t *s, uint8_t reg, uint8_t *buf, size_t len);
esp_err_t sensor_write_reg(sensor_inst_t *s, uint8_t reg, const uint8_t *buf, size_t len);
void sensor_publish(sensor_inst_t *s, const char *quantity, float value);
// for drivers with several measuring points (1-Wire probes): own instance name per point
void sensor_publish_as(const char *instance, const char *quantity, float value);
// poll this instance as soon as possible
void sensors_kick(sensor_inst_t *s);

extern const sensor_driver_t sensor_drv_bh1750;
extern const sensor_driver_t sensor_drv_bme68x;   // bme68x_bsec.c
extern const sensor_driver_t sensor_drv_ds2484;
extern const sensor_driver_t sensor_drv_bme69x;
extern const sensor_driver_t sensor_drv_sht4x;
extern const sensor_driver_t sensor_drv_scd4x;

// Sensirion CRC-8 (poly 0x31, init 0xFF) over one 16-bit word
uint8_t sensirion_crc8(const uint8_t *data, size_t len);
