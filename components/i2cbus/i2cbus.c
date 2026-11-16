#include "i2cbus.h"

#include "board.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "i2cbus";
static i2c_master_bus_handle_t s_bus;

esp_err_t i2cbus_init(void)
{
    const i2c_master_bus_config_t cfg = {
        .i2c_port = -1,                 // any free controller
        .sda_io_num = BOARD_I2C_SDA_GPIO,
        .scl_io_num = BOARD_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,   // weak (~45 kOhm); the breakouts carry the real pull-ups
    };
    esp_err_t err = i2c_new_master_bus(&cfg, &s_bus);
    ESP_LOGI(TAG, "bus on SDA=GPIO%d SCL=GPIO%d %lu Hz: %s", BOARD_I2C_SDA_GPIO, BOARD_I2C_SCL_GPIO,
             (unsigned long)CONFIG_HUB_I2C_FREQ_HZ, esp_err_to_name(err));
    return err;
}

i2c_master_bus_handle_t i2cbus_handle(void)
{
    return s_bus;
}

uint32_t i2cbus_freq_hz(void)
{
    return CONFIG_HUB_I2C_FREQ_HZ;
}

// Work-around for spurious probe results in ESP-IDF v6.1.1 (observed 2026-11-17: in ~50 % of
// back-to-back scans phantom ACKs at random addresses, and a real device reported at the
// neighbour address, e.g. 0x75 instead of 0x76). Likely cause (ASSUMPTION from reading
// i2c_master.c): after a NACK a late interrupt (STOP complete) queues an event that the next
// probe consumes. A 1 ms pause lets that interrupt land on its own probe.
esp_err_t i2cbus_probe(uint8_t addr)
{
    esp_err_t err = i2c_master_probe(s_bus, addr, 50);
    vTaskDelay(pdMS_TO_TICKS(1));
    return err;
}

int i2cbus_scan(uint8_t found[128])
{
    int n = 0;
    for (int a = 0; a < 128; a++) {
        found[a] = 0;
        if (a < 0x08 || a > 0x77) {
            continue;
        }
        if (i2cbus_probe(a) == ESP_OK) {
            found[a] = 1;
            n++;
        }
    }
    return n;
}
