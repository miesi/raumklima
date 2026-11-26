// The I2C master buses (1 or 2, see board.h). Bus 0 = IO3/IO8, bus 1 = IO15/IO16.
#pragma once

#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t i2cbus_init(void);

int i2cbus_count(void);

i2c_master_bus_handle_t i2cbus_handle(int bus);

uint32_t i2cbus_freq_hz(void);

// ACK check of one address (use this, not i2c_master_probe directly - see i2cbus.c).
esp_err_t i2cbus_probe(int bus, uint8_t addr);

// Probes 0x08..0x77; found[addr] = 1 for every ACK. Returns number found.
int i2cbus_scan(int bus, uint8_t found[128]);

#ifdef __cplusplus
}
#endif
