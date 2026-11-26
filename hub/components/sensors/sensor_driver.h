// Interface every sensor driver implements. Adding a sensor = new file in drivers/ plus
// one line in the registry in sensors.c. A sensor with its own procedures (calibration, sub-devices)
// also exports sensors_<name>_* functions in sensors.h - see "Architecture rules" in CLAUDE.md.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

typedef struct sensor_inst sensor_inst_t;

// poll() of a sensor without a cadence: the read cycle is complete, values are published
#define SENSOR_DONE 0

typedef struct {
    const char *driver;            // e.g. "bme68x"
    const uint8_t *addrs;          // candidate I2C addresses
    uint8_t n_addrs;
    // Identify the chip at s->addr (s->dev is ready). Fill s->name, s->model, s->supported,
    // s->ctx. Return ESP_ERR_NOT_FOUND if this is not our chip.
    esp_err_t (*probe)(sensor_inst_t *s);
    // Measuring cadence the sensor needs (us), e.g. BSEC 3 s, SCD4x periodic mode 5 s; the driver
    // polls itself at that pace. 0 = no cadence: the registry starts one read cycle per sensor
    // interval (the shorter of the display and the Carbon interval), timed to end 100 ms before
    // the boundary.
    int64_t cadence_us;
    const char *cadence_why;       // with a cadence: why (console help)
    // Do one step of work, publish metrics.
    // With a cadence: return the esp_timer time (us) of the next call.
    // Without: return SENSOR_DONE when the cycle's values are published, or the esp_timer time
    // of the next step of the same cycle (e.g. after a conversion time).
    // On error: return sensor_fail(s, err); the registry logs it, counts it and retries in 5 s
    // (with a cadence) or at the next cycle (without).
    int64_t (*poll)(sensor_inst_t *s, int64_t now_us);
    // optional, sensors without a cadence: slow housekeeping after a read cycle, outside the
    // timed part (1-Wire search)
    void (*after_cycle)(sensor_inst_t *s);
    void (*save_state)(sensor_inst_t *s);    // optional
    // optional: multi-line text for the display/console detail page (e.g. one line per 1-Wire probe)
    void (*detail)(sensor_inst_t *s, char *buf, size_t len);
} sensor_driver_t;

struct sensor_inst {
    const sensor_driver_t *drv;
    i2c_master_dev_handle_t dev;
    uint8_t addr;
    uint8_t bus;           // 0 = IO3/IO8, 1 = IO15/IO16
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
    esp_err_t last_err;    // set by sensor_fail()
    uint32_t fail_streak;  // failed polls in a row
    // sensors without a cadence: the current read cycle
    bool cycle_busy;
    int64_t cycle_start_us;    // first poll of the cycle, 0 = not polled yet
    uint32_t read_ms;          // duration of the last complete cycle
    uint32_t read_ms_max;      // since boot
    bool kicked;               // sensors_kick(): poll / housekeeping as soon as possible
};

// for poll(): remember the error for the log, return the error value
static inline int64_t sensor_fail(sensor_inst_t *s, esp_err_t err)
{
    s->last_err = err;
    return -1;
}

// helpers for drivers
esp_err_t sensor_read_reg(sensor_inst_t *s, uint8_t reg, uint8_t *buf, size_t len);
esp_err_t sensor_write_reg(sensor_inst_t *s, uint8_t reg, const uint8_t *buf, size_t len);
void sensor_publish(sensor_inst_t *s, const char *quantity, float value);
// a state (accuracy, status, setting): not averaged, Carbon sends the last value
void sensor_publish_state(sensor_inst_t *s, const char *quantity, float value);
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
