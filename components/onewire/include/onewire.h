// 1-Wire bus logic independent of the bus master hardware (DS2484 today).
//
// Data model: up to OW_MAX_DEVICES devices per bus, identified by their 64-bit ROM ID.
// Each device can get a name on the console (NVS); the name is the metric instance, e.g.
// "flur_vorlauf.temperature_c". Unnamed devices use "ow_<rom>".
// More devices than OW_MAX_DEVICES: the search keeps counting (up to OW_SEARCH_LIMIT) so the
// error message can say how many there are; the first OW_MAX_DEVICES in search order are used.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OW_MAX_DEVICES   8
#define OW_SEARCH_LIMIT  32
#define OW_NAME_LEN      16     // incl. NUL: max 15 chars [a-z0-9_]
#define OW_NAMES_MAX     16     // stored names; more than devices, so spare probes keep theirs
#define OW_ROM_STR_LEN   17

// ROM commands
#define OW_CMD_SEARCH_ROM  0xF0
#define OW_CMD_MATCH_ROM   0x55
#define OW_CMD_SKIP_ROM    0xCC

// Family codes
#define OW_FAMILY_DS18B20  0x28

// Bus master operations. ROM bytes travel LSB first; a ROM is stored as uint64_t with
// byte 0 (family code) in the low byte.
typedef struct {
    esp_err_t (*reset)(void *ctx, bool *presence);
    esp_err_t (*write_byte)(void *ctx, uint8_t b);
    esp_err_t (*read_byte)(void *ctx, uint8_t *b);
    // one search step: read bit, read complement, write the chosen direction
    esp_err_t (*triplet)(void *ctx, bool dir, uint8_t *id_bit, uint8_t *cmp_bit, uint8_t *taken);
    void *ctx;
} ow_bus_t;

// Dallas/Maxim CRC-8 (x^8 + x^5 + x^4 + 1), as used for ROM IDs and the DS18B20 scratchpad.
uint8_t ow_crc8(const uint8_t *data, size_t len);

// Search ROM over the whole bus. Stores at most max ROMs in search order.
// Returns the number of devices found (can be > max), or -1 on a bus error.
int ow_search(const ow_bus_t *bus, uint64_t *roms, int max, uint32_t *crc_errors);

esp_err_t ow_select(const ow_bus_t *bus, uint64_t rom);   // reset + Match ROM

uint8_t ow_family(uint64_t rom);
void ow_rom_str(uint64_t rom, char out[OW_ROM_STR_LEN]);  // 16 hex digits, family code first
bool ow_rom_parse(const char *s, uint64_t *rom);

// --- names (NVS) ---
esp_err_t ow_names_init(void);
// Writes the name, or "ow_<rom>" if there is none. Returns true if a name is set.
bool ow_name_get(uint64_t rom, char *out, size_t len);
// name NULL or "" removes it. Saved to NVS immediately.
esp_err_t ow_name_set(uint64_t rom, const char *name);
// Copies stored names, returns count.
int ow_names_list(uint64_t *roms, char (*names)[OW_NAME_LEN], int max);

// --- self test on a simulated bus with n devices (no hardware needed) ---
// Writes a human readable report; returns true if the search and the limit behaved.
bool ow_selftest(int n_devices, char *report, size_t len);

#ifdef __cplusplus
}
#endif
