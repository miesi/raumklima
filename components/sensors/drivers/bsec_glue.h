// Thin layer over BSEC2 (global state, BME680) and BSEC3 (instances, BME680 + BME690).
// HUB_BSEC_VERSION is set by the bosch_bme68x component.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bsec_interface.h"
#include "esp_err.h"
#include "sensor_driver.h"

typedef enum { BSEC_CHIP_BME680, BSEC_CHIP_BME690 } bsec_chip_t;

typedef struct bsecg bsecg_t;

// One forced measurement with BSEC's settings.
typedef struct {
    float temperature, pressure, humidity, gas_resistance;
    bool new_data, gas_valid;
} bme_sample_t;
typedef esp_err_t (*bme_measure_fn)(void *chip, const bsec_bme_settings_t *st, bme_sample_t *out);

typedef struct {
    bsecg_t *bsec;
    bme_measure_fn measure;
    void *chip;
    char version[24];
    char nvs_key[12];
    uint8_t iaq_accuracy;
    int64_t last_save_us;
} bme_bsec_t;

int bsecg_version(void);

// Sets up BSEC for the chip; false if this build cannot (e.g. BME690 with BSEC2).
bool bme_bsec_init(bme_bsec_t *b, bsec_chip_t chip, bme_measure_fn measure, void *chip_ctx, const char **why);

// One BSEC step: measure if BSEC asks, feed, publish. Returns the next call time (us), <0 on error.
int64_t bme_bsec_poll(sensor_inst_t *s, bme_bsec_t *b, int64_t now_us);

void bme_bsec_save(bme_bsec_t *b);
