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
    uint8_t bus;          // 0 or 1 (shown as 1 / 2)
    bool supported;       // false: detected, but no driver for it (yet)
    uint32_t ok_count;
    uint32_t err_count;
    int64_t last_ok_us;
    char info[48];        // driver-specific state, e.g. "BSEC 2.6.1.0, IAQ acc 3"
    int64_t cadence_us;   // own measuring cadence, 0 = read once per sensor interval
    uint32_t read_ms;     // without cadence: duration of the last read cycle
    uint32_t read_ms_max;
    int last_err;         // esp_err_t of the last failed poll, 0 = none
} sensor_info_t;

// Most I2C sensor instances the firmware handles. More found: error message, the first
// SENSORS_MAX (in probe order) are used.
#define SENSORS_MAX CONFIG_HUB_SENSORS_MAX

// Probes and starts the polling task. Needs i2cbus_init() and metrics_init().
esp_err_t sensors_start(void);

// Copies up to max entries, returns count.
int sensors_list(sensor_info_t *out, int max);

// Pauses all driver polling (multi-step sequences like command - wait - read included) so a
// bus scan does not land between the steps.
void sensors_bus_lock(void);
void sensors_bus_unlock(void);

// Sensor interval: sensors without a cadence of their own are read once per interval, the
// shorter of the display and the Carbon interval.
int sensors_interval_s(void);
// Longest recent read cycle of all sensors without a cadence (ms), 0 = none run yet. The
// display and Carbon intervals must not be shorter than this plus the 100 ms lead.
uint32_t sensors_read_ms(void);
// Plan the next read cycle again (intervals changed, clock set).
void sensors_reschedule(void);
// Help text: every supported sensor type with its cadence and why, or "once per interval".
void sensors_cadence_help(char *buf, size_t len);

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

// --- SCD4x (CO2) ---
// All block until the sensors task has done it (max 10 s); msg gets a human readable result.
esp_err_t sensors_scd4x_offset_auto(char *msg, size_t len);          // against SHT4x
esp_err_t sensors_scd4x_offset_set(float c, char *msg, size_t len);
esp_err_t sensors_scd4x_offset_reset(char *msg, size_t len);
#define SCD4X_MSG_LEN 512
esp_err_t sensors_scd4x_diag(char *msg, size_t len);               // settings + 10 s self test
esp_err_t sensors_scd4x_single_shot(char *msg, size_t len);        // one measurement outside periodic mode
esp_err_t sensors_scd4x_reinit(bool sleep_cycle, char *msg, size_t len);   // reinit or power_down/wake_up
esp_err_t sensors_scd4x_asc(bool on, char *msg, size_t len);       // RAM only
esp_err_t sensors_scd4x_factory_reset(char *msg, size_t len);      // erases FRC/ASC history - irreversible
void sensors_scd4x_log_raw(bool on);                               // print every raw reading        // back to the sensor's EEPROM value
esp_err_t sensors_scd4x_frc(uint16_t ppm, bool force, char *msg, size_t len);
esp_err_t sensors_scd4x_set_outdoor(uint16_t ppm);                   // 380..500, NVS
// "I am in fresh air": counts only readings from now on, waits until they were stable for 3 min
// (max 30 min), then FRC to the outdoor value unless the mean is more than 150 ppm away from it.
esp_err_t sensors_scd4x_fresh_air_start(void);
// Days since the last FRC; false if never or no SCD4x.
bool sensors_scd4x_calibration_age(int *days);
typedef struct {
    uint32_t good_streak;   // consecutive plausible CO2 readings (0 = last one was a fault)
    uint32_t readings;      // all readings since boot
    uint32_t faults;        // implausible readings since boot
} scd4x_co2_state_t;
// False if there is no SCD4x.
bool sensors_scd4x_co2_state(scd4x_co2_state_t *st);
// Health check on implausible CO2 readings, at most every <minutes> (0 = off); the first
// fault after boot is checked right away. Self test, on failure power_down/wake_up and a
// second self test. NVS. Each check stops the measurement ~11-23 s.
esp_err_t sensors_scd4x_health_interval(int minutes);
void sensors_scd4x_health_print(void);

// Saves driver state that must survive a reboot (BSEC). Call before a planned restart.
// Waits until no driver is polled (sensors_bus_lock()), up to ~11 s during 'scd41 diag'.
void sensors_save_state(void);

#ifdef __cplusplus
}
#endif
