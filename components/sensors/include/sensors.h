// Sensor registry: probes all known drivers on the I2C bus and polls them.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char name[16];        // instance name, prefix of the metric keys
    char model[24];       // what was detected, e.g. "BME680", "BME690"
    uint8_t addr;
    bool supported;       // false: detected, but no driver for it (yet)
    uint32_t ok_count;
    uint32_t err_count;
    int64_t last_ok_us;
    char info[48];        // driver-specific state, e.g. "BSEC 2.6.1.0, IAQ acc 3"
} sensor_info_t;

// Probes and starts the polling task. Needs i2cbus_init() and metrics_init().
esp_err_t sensors_start(void);

// Copies up to max entries, returns count.
int sensors_list(sensor_info_t *out, int max);

// Saves driver state that must survive a reboot (BSEC). Call before a planned restart.
void sensors_save_state(void);

#ifdef __cplusplus
}
#endif
