// Driver for two cascaded SSD1683 (Solomon Systech SSD1683 Rev 1.0 datasheet).
//
// Geometry (from the Elecrow and GxEPD2 reference code for this panel; facts only, no code copied):
// - The slave controller drives the left 396 columns, the master the right 396 columns.
// - Commands with bit 7 set address the slave (0xA4 = slave "Write RAM BW", 0x91 = slave
//   "Data Entry Mode" ...). Update commands (0x20/0x21/0x22) go to the master, which runs
//   the slave through the cascade line (0x21 B[4] = cascade application).
// - Slave RAM is written with X increment, master RAM with X decrement (mirrored source
//   order). Each controller gets 50 bytes per row; image bytes 0..49 go to the slave and
//   49..98 to the master (byte 49 holds the seam).
//
// SPI write clock max 20 MHz (datasheet "fSCL Write Mode"); we use 10 MHz.
#include "epd.h"

#include <string.h>

#include "board.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "epd";

#define SPI_HOST_ID     SPI2_HOST
#define SPI_HZ          (10 * 1000 * 1000)
#define CTRL_COLS       396
#define CTRL_BYTES      50                  // (396 + 7) / 8
#define PLANE_BYTES     (CTRL_BYTES * EPD_HEIGHT)
#define SLAVE           0x80
#define BUSY_TIMEOUT_MS 6000

// commands (SSD1683 command table)
#define CMD_SW_RESET        0x12
#define CMD_TEMP_SENSOR     0x18
#define CMD_DATA_ENTRY      0x11
#define CMD_RAM_X_RANGE     0x44
#define CMD_RAM_Y_RANGE     0x45
#define CMD_RAM_X_COUNTER   0x4E
#define CMD_RAM_Y_COUNTER   0x4F
#define CMD_WRITE_RAM_BW    0x24
#define CMD_WRITE_RAM_RED   0x26            // used as "previous image" in B/W display mode 2
#define CMD_UPDATE_CTRL1    0x21
#define CMD_UPDATE_CTRL2    0x22
#define CMD_MASTER_ACT      0x20
#define CMD_BORDER          0x3C
#define CMD_DEEP_SLEEP      0x10

static spi_device_handle_t s_spi;
static uint8_t *s_plane;   // DMA-capable staging buffer, one controller plane
static epd_stats_t s_stats;

static void spi_send(const uint8_t *data, size_t len)
{
    spi_transaction_t t = {.length = len * 8, .tx_buffer = data};
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));
}

static void cmd(uint8_t c)
{
    gpio_set_level(BOARD_EPD_DC_GPIO, 0);
    spi_send(&c, 1);
}

static void data(const uint8_t *d, size_t len)
{
    gpio_set_level(BOARD_EPD_DC_GPIO, 1);
    spi_send(d, len);
}

static void cmd_data(uint8_t c, const uint8_t *d, size_t len)
{
    cmd(c);
    if (len) {
        data(d, len);
    }
}

static bool wait_busy(uint32_t *elapsed_ms)
{
    int64_t t0 = esp_timer_get_time();
    while (gpio_get_level(BOARD_EPD_BUSY_GPIO) == 1) {
        if (esp_timer_get_time() - t0 > BUSY_TIMEOUT_MS * 1000LL) {
            s_stats.busy_timeouts++;
            ESP_LOGE(TAG, "BUSY timeout");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (elapsed_ms) {
        *elapsed_ms = (esp_timer_get_time() - t0) / 1000;
    }
    return true;
}

static void hw_reset(void)
{
    gpio_set_level(BOARD_EPD_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(BOARD_EPD_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    wait_busy(NULL);
}

// full-panel RAM window and address counters for one controller
static void set_window(bool slave)
{
    const uint8_t t = slave ? SLAVE : 0;
    const uint8_t y_end_lo = (EPD_HEIGHT - 1) & 0xff, y_end_hi = (EPD_HEIGHT - 1) >> 8;
    const uint8_t y_range[4] = {0, 0, y_end_lo, y_end_hi};
    const uint8_t y_cnt[2] = {0, 0};
    if (slave) {
        const uint8_t mode = 0x03;                       // X increment, Y increment
        const uint8_t x_range[2] = {0, CTRL_BYTES - 1};
        const uint8_t x_cnt = 0;
        cmd_data(CMD_DATA_ENTRY | t, &mode, 1);
        cmd_data(CMD_RAM_X_RANGE | t, x_range, 2);
        cmd_data(CMD_RAM_Y_RANGE | t, y_range, 4);
        cmd_data(CMD_RAM_X_COUNTER | t, &x_cnt, 1);
        cmd_data(CMD_RAM_Y_COUNTER | t, y_cnt, 2);
    } else {
        const uint8_t mode = 0x02;                       // X decrement, Y increment
        const uint8_t x_range[2] = {CTRL_BYTES - 1, 0};
        const uint8_t x_cnt = CTRL_BYTES - 1;
        cmd_data(CMD_DATA_ENTRY | t, &mode, 1);
        cmd_data(CMD_RAM_X_RANGE | t, x_range, 2);
        cmd_data(CMD_RAM_Y_RANGE | t, y_range, 4);
        cmd_data(CMD_RAM_X_COUNTER | t, &x_cnt, 1);
        cmd_data(CMD_RAM_Y_COUNTER | t, y_cnt, 2);
    }
}

// copies one controller's half of the frame into the staging buffer and sends it
static void write_plane(uint8_t ram_cmd, const uint8_t *fb, int stride, bool slave)
{
    const int first = slave ? 0 : CTRL_BYTES - 1;   // byte 49 is shared at the seam
    for (int y = 0; y < EPD_HEIGHT; y++) {
        memcpy(&s_plane[y * CTRL_BYTES], &fb[y * stride + first], CTRL_BYTES);
    }
    set_window(slave);
    cmd(ram_cmd | (slave ? SLAVE : 0));
    data(s_plane, PLANE_BYTES);
}

static void write_ram(uint8_t ram_cmd, const uint8_t *fb, int stride)
{
    write_plane(ram_cmd, fb, stride, false);   // master first: plain commands may reach both
    write_plane(ram_cmd, fb, stride, true);
}

esp_err_t epd_init(void)
{
    const gpio_config_t out = {
        .pin_bit_mask = (1ULL << BOARD_EPD_DC_GPIO) | (1ULL << BOARD_EPD_RST_GPIO),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out);
    const gpio_config_t in = {.pin_bit_mask = 1ULL << BOARD_EPD_BUSY_GPIO, .mode = GPIO_MODE_INPUT};
    gpio_config(&in);
    gpio_set_level(BOARD_EPD_RST_GPIO, 1);

    const spi_bus_config_t bus = {
        .mosi_io_num = BOARD_EPD_MOSI_GPIO,
        .miso_io_num = -1,
        .sclk_io_num = BOARD_EPD_SCK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = PLANE_BYTES,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI_HOST_ID, &bus, SPI_DMA_CH_AUTO));
    const spi_device_interface_config_t dev = {
        .clock_speed_hz = SPI_HZ,
        .mode = 0,
        .spics_io_num = BOARD_EPD_CS_GPIO,
        .queue_size = 2,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI_HOST_ID, &dev, &s_spi));

    s_plane = heap_caps_malloc(PLANE_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_plane) {
        return ESP_ERR_NO_MEM;
    }

    hw_reset();
    cmd(CMD_SW_RESET);
    vTaskDelay(pdMS_TO_TICKS(10));
    if (!wait_busy(NULL)) {
        return ESP_ERR_TIMEOUT;
    }
    const uint8_t internal_sensor = 0x80;
    cmd_data(CMD_TEMP_SENSOR, &internal_sensor, 1);
    ESP_LOGI(TAG, "SSD1683 x2 ready, SPI %d MHz", SPI_HZ / 1000000);
    return ESP_OK;
}

esp_err_t epd_show(const uint8_t *fb, int stride, bool full)
{
    uint32_t busy_ms = 0;
    int64_t t0 = esp_timer_get_time();
    if (full) {
        write_ram(CMD_WRITE_RAM_BW, fb, stride);
        write_ram(CMD_WRITE_RAM_RED, fb, stride);     // previous = current, for the next partial
        s_stats.last_spi_ms = (esp_timer_get_time() - t0) / 1000;
        const uint8_t ctrl1[2] = {0x40, 0x10};        // RED RAM bypassed as 0, cascade application
        const uint8_t ctrl2 = 0xF7;                   // clock, analog, load temp + LUT, display, off
        cmd_data(CMD_UPDATE_CTRL1, ctrl1, 2);
        cmd_data(CMD_UPDATE_CTRL2, &ctrl2, 1);
        cmd(CMD_MASTER_ACT);
        if (!wait_busy(&busy_ms)) {
            return ESP_ERR_TIMEOUT;
        }
        s_stats.full_count++;
    } else {
        write_ram(CMD_WRITE_RAM_BW, fb, stride);
        uint32_t spi_ms = (esp_timer_get_time() - t0) / 1000;
        const uint8_t border = 0x80;                  // border follows VCOM, no flicker
        const uint8_t ctrl1[2] = {0x00, 0x10};        // RED RAM normal (= previous image), cascade
        const uint8_t ctrl2 = 0xFF;                   // as F7, but display mode 2: old/new compare
        cmd_data(CMD_BORDER, &border, 1);
        cmd_data(CMD_UPDATE_CTRL1, ctrl1, 2);
        cmd_data(CMD_UPDATE_CTRL2, &ctrl2, 1);
        cmd(CMD_MASTER_ACT);
        if (!wait_busy(&busy_ms)) {
            return ESP_ERR_TIMEOUT;
        }
        int64_t t1 = esp_timer_get_time();
        write_ram(CMD_WRITE_RAM_RED, fb, stride);     // what is on the panel now
        s_stats.last_spi_ms = spi_ms + (esp_timer_get_time() - t1) / 1000;
        s_stats.partial_count++;
    }
    s_stats.last_refresh_ms = busy_ms;
    ESP_LOGI(TAG, "%s refresh: SPI %lu ms, panel %lu ms", full ? "full" : "partial",
             (unsigned long)s_stats.last_spi_ms, (unsigned long)busy_ms);
    return ESP_OK;
}

void epd_get_stats(epd_stats_t *out)
{
    *out = s_stats;
}
