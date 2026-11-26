#include "onewire.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

uint8_t ow_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];
        for (int j = 0; j < 8; j++) {
            uint8_t mix = (crc ^ b) & 0x01;
            crc >>= 1;
            if (mix) {
                crc ^= 0x8C;   // reflected 0x31
            }
            b >>= 1;
        }
    }
    return crc;
}

static void rom_bytes(uint64_t rom, uint8_t b[8])
{
    for (int i = 0; i < 8; i++) {
        b[i] = (rom >> (8 * i)) & 0xff;
    }
}

uint8_t ow_family(uint64_t rom)
{
    return rom & 0xff;
}

void ow_rom_str(uint64_t rom, char out[OW_ROM_STR_LEN])
{
    uint8_t b[8];
    rom_bytes(rom, b);
    for (int i = 0; i < 8; i++) {
        snprintf(out + 2 * i, 3, "%02x", b[i]);
    }
}

bool ow_rom_parse(const char *s, uint64_t *rom)
{
    if (strlen(s) != 16) {
        return false;
    }
    uint64_t r = 0;
    for (int i = 0; i < 8; i++) {
        unsigned v;
        char tmp[3] = {s[2 * i], s[2 * i + 1], 0};
        if (!isxdigit((unsigned char)tmp[0]) || !isxdigit((unsigned char)tmp[1]) || sscanf(tmp, "%2x", &v) != 1) {
            return false;
        }
        r |= (uint64_t)v << (8 * i);
    }
    *rom = r;
    return true;
}

// Search ROM with the triplet primitive (algorithm of Maxim application note 187).
int ow_search(const ow_bus_t *bus, uint64_t *roms, int max, uint32_t *crc_errors)
{
    int found = 0;
    int last_discrepancy = 0;   // 1-based bit position, 0 = none
    uint64_t rom = 0;

    for (;;) {
        bool presence = false;
        if (bus->reset(bus->ctx, &presence) != ESP_OK) {
            return -1;
        }
        if (!presence) {
            return found;       // empty bus (or all devices gone)
        }
        if (bus->write_byte(bus->ctx, OW_CMD_SEARCH_ROM) != ESP_OK) {
            return -1;
        }
        int last_zero = 0;
        uint64_t next = 0;
        for (int bit = 1; bit <= 64; bit++) {
            bool dir;
            if (bit < last_discrepancy) {
                dir = (rom >> (bit - 1)) & 1;   // follow the previous path
            } else {
                dir = bit == last_discrepancy;  // at the last fork take 1 this time
            }
            uint8_t id, cmp, taken;
            if (bus->triplet(bus->ctx, dir, &id, &cmp, &taken) != ESP_OK) {
                return -1;
            }
            if (id && cmp) {
                return found;   // nobody answered (device left the bus mid-search)
            }
            if (!id && !cmp && !taken) {
                last_zero = bit;
            }
            if (taken) {
                next |= 1ULL << (bit - 1);
            }
        }
        uint8_t b[8];
        rom_bytes(next, b);
        if (ow_crc8(b, 7) != b[7]) {
            if (crc_errors) {
                (*crc_errors)++;
            }
            return found > 0 ? found : -1;
        }
        if (found < max) {
            roms[found] = next;
        }
        found++;
        rom = next;
        last_discrepancy = last_zero;
        if (last_discrepancy == 0 || found >= OW_SEARCH_LIMIT) {
            return found;
        }
    }
}

esp_err_t ow_select(const ow_bus_t *bus, uint64_t rom)
{
    bool presence = false;
    esp_err_t err = bus->reset(bus->ctx, &presence);
    if (err != ESP_OK) {
        return err;
    }
    if (!presence) {
        return ESP_ERR_NOT_FOUND;
    }
    err = bus->write_byte(bus->ctx, OW_CMD_MATCH_ROM);
    for (int i = 0; i < 8 && err == ESP_OK; i++) {
        err = bus->write_byte(bus->ctx, (rom >> (8 * i)) & 0xff);
    }
    return err;
}
