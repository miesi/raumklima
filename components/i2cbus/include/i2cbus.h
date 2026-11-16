// The shared I2C master bus. One handle, used by all sensor drivers.
#pragma once

#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t i2cbus_init(void);

i2c_master_bus_handle_t i2cbus_handle(void);

uint32_t i2cbus_freq_hz(void);

// ACK check of one address (use this, not i2c_master_probe directly - see i2cbus.c).
esp_err_t i2cbus_probe(uint8_t addr);

// Probes 0x08..0x77; found[addr] = 1 for every ACK. Returns number found.
int i2cbus_scan(uint8_t found[128]);

#ifdef __cplusplus
}
#endif
