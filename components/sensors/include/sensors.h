// Sensor registry: probes all known drivers on the I2C bus and polls them.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "sdkconfig.h"
#include "esp_err.h"
#include "onewire.h"

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

// Most I2C sensor instances the firmware handles. More found: error message, the first
// SENSORS_MAX (in probe order) are used.
#define SENSORS_MAX CONFIG_HUB_SENSORS_MAX

// Probes and starts the polling task. Needs i2cbus_init() and metrics_init().
esp_err_t sensors_start(void);

// Copies up to max entries, returns count.
int sensors_list(sensor_info_t *out, int max);

// Driver detail text for sensor idx (as listed); false if the driver has none.
bool sensors_detail(int idx, char *buf, size_t len);

// Problems to show on the display, e.g. "9 1-Wire devices, max 8". Empty string = none.
void sensors_warnings(char *buf, size_t len);

// --- 1-Wire (DS2484) ---
typedef struct {
    uint64_t rom;
    char name[OW_INSTANCE_LEN];
    float temp_c;
    bool valid;
    uint32_t crc_errors;
} sensors_ow_dev_t;

// Devices in use (max OW_MAX_DEVICES). found = devices on the bus. -1 = no DS2484.
int sensors_onewire_list(sensors_ow_dev_t *out, int max, int *found);
// Requests a new search; returns the scan counter before the request.
uint32_t sensors_onewire_rescan(void);
uint32_t sensors_onewire_scan_count(void);

// Saves driver state that must survive a reboot (BSEC). Call before a planned restart.
void sensors_save_state(void);

#ifdef __cplusplus
}
#endif
