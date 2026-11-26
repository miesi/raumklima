// Simulated 1-Wire bus for ow_selftest(): wired-AND search semantics without hardware.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "onewire.h"

typedef struct {
    uint64_t roms[OW_SEARCH_LIMIT + 8];
    int n;
    bool active[OW_SEARCH_LIMIT + 8];
    int bit;
} sim_t;

static esp_err_t sim_reset(void *ctx, bool *presence)
{
    sim_t *s = ctx;
    *presence = s->n > 0;
    for (int i = 0; i < s->n; i++) {
        s->active[i] = true;
    }
    s->bit = 0;
    return ESP_OK;
}

static esp_err_t sim_write(void *ctx, uint8_t b)
{
    return ESP_OK;
}

static esp_err_t sim_read(void *ctx, uint8_t *b)
{
    *b = 0xff;
    return ESP_OK;
}

static esp_err_t sim_triplet(void *ctx, bool dir, uint8_t *id, uint8_t *cmp, uint8_t *taken)
{
    sim_t *s = ctx;
    bool has0 = false, has1 = false;
    for (int i = 0; i < s->n; i++) {
        if (s->active[i]) {
            if ((s->roms[i] >> s->bit) & 1) {
                has1 = true;
            } else {
                has0 = true;
            }
        }
    }
    *id = !has0;    // open drain: reads 1 only if nobody pulls low
    *cmp = !has1;
    if (*id != *cmp) {
        *taken = *id;
    } else {
        *taken = *id ? 1 : dir;
    }
    for (int i = 0; i < s->n; i++) {
        if (s->active[i] && (((s->roms[i] >> s->bit) & 1) != *taken)) {
            s->active[i] = false;
        }
    }
    s->bit++;
    return ESP_OK;
}

bool ow_selftest(int n, char *report, size_t len)
{
    if (n < 0 || n > OW_SEARCH_LIMIT + 8) {
        snprintf(report, len, "n must be 0..%d\n", OW_SEARCH_LIMIT + 8);
        return false;
    }
    sim_t *s = calloc(1, sizeof(sim_t));
    if (!s) {
        return false;
    }
    // CRC check against the example ROM of Maxim application note 27: 02 1C B8 01 00 00 00 A2
    const uint8_t an27[8] = {0x02, 0x1C, 0xB8, 0x01, 0x00, 0x00, 0x00, 0xA2};
    bool crc_ok = ow_crc8(an27, 7) == 0xA2;

    uint32_t seed = 12345;
    for (int i = 0; i < n; i++) {
        uint8_t b[8] = {OW_FAMILY_DS18B20};
        for (int j = 1; j < 7; j++) {
            seed = seed * 1103515245u + 12345u;
            b[j] = seed >> 16;
        }
        b[7] = ow_crc8(b, 7);
        uint64_t r = 0;
        for (int j = 0; j < 8; j++) {
            r |= (uint64_t)b[j] << (8 * j);
        }
        s->roms[i] = r;
    }
    s->n = n;
    ow_bus_t bus = {sim_reset, sim_write, sim_read, sim_triplet, s};
    uint64_t roms[OW_MAX_DEVICES];
    uint32_t crc_err = 0;
    int found = ow_search(&bus, roms, OW_MAX_DEVICES, &crc_err);
    int stored = found < OW_MAX_DEVICES ? found : OW_MAX_DEVICES;

    // every stored ROM must be one of the simulated ones and appear once
    bool ids_ok = true;
    for (int i = 0; i < stored; i++) {
        bool known = false;
        for (int j = 0; j < n; j++) {
            known |= roms[i] == s->roms[j];
        }
        for (int k = 0; k < i; k++) {
            ids_ok &= roms[k] != roms[i];
        }
        ids_ok &= known;
    }
    int expect = n > OW_SEARCH_LIMIT ? OW_SEARCH_LIMIT : n;
    bool ok = crc_ok && ids_ok && found == expect;

    size_t o = snprintf(report, len, "CRC-8 vs AN27 example: %s\nsimulated %d, found %d, stored %d, IDs %s\n",
                        crc_ok ? "ok" : "FAIL", n, found, stored, ids_ok ? "ok" : "FAIL");
    if (found > OW_MAX_DEVICES && o < len) {
        o += snprintf(report + o, len - o, "ERROR: %d%s 1-Wire devices found, max %d - using the first %d\n", found,
                      found >= OW_SEARCH_LIMIT ? "+" : "",
                      OW_MAX_DEVICES, OW_MAX_DEVICES);
    }
    for (int i = 0; i < stored && o < len; i++) {
        char r[OW_ROM_STR_LEN];
        ow_rom_str(roms[i], r);
        o += snprintf(report + o, len - o, "  %d %s\n", i + 1, r);
    }
    if (o < len) {
        snprintf(report + o, len - o, "%s\n", ok ? "PASS" : "FAIL");
    }
    free(s);
    return ok;
}
