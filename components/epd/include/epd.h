// 5.79" e-paper, 792 x 272, two cascaded SSD1683 controllers (CrowPanel 5.79 / GDEY0579T93).
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EPD_WIDTH  792
#define EPD_HEIGHT 272

esp_err_t epd_init(void);

// fb: 1 bpp, row-major, MSB = leftmost pixel, bit set = white, stride >= 99 bytes.
// full: full refresh (flashes, ~2.2 s, clears ghosting). Otherwise partial refresh: the
// controller compares new (RAM 0x24) with previous (RAM 0x26) and drives only changed pixels.
// Blocks until the panel is done.
esp_err_t epd_show(const uint8_t *fb, int stride, bool full);

typedef struct {
    uint32_t full_count;
    uint32_t partial_count;
    uint32_t last_refresh_ms;   // BUSY time of the last refresh
    uint32_t last_spi_ms;       // SPI time of the last frame (both RAMs, both controllers)
    uint32_t busy_timeouts;
} epd_stats_t;

void epd_get_stats(epd_stats_t *out);

#ifdef __cplusplus
}
#endif
