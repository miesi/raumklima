// Board abstraction. The only place in the firmware that knows GPIO numbers.
#pragma once

#include <stdbool.h>
#include "driver/gpio.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#if CONFIG_HUB_BOARD_CROWPANEL_579
// Source for every pin: Elecrow schematic, see docs/hardware-crowpanel-579.md
#define BOARD_NAME            "CrowPanel 5.79"
#define BOARD_EPD_PWR_GPIO    GPIO_NUM_7    // IO7_LCD_3.3_CTL, high = panel powered
#define BOARD_EPD_MOSI_GPIO   GPIO_NUM_11
#define BOARD_EPD_SCK_GPIO    GPIO_NUM_12
#define BOARD_EPD_CS_GPIO     GPIO_NUM_45
#define BOARD_EPD_DC_GPIO     GPIO_NUM_46
#define BOARD_EPD_RST_GPIO    GPIO_NUM_47
#define BOARD_EPD_BUSY_GPIO   GPIO_NUM_48   // high = busy
#define BOARD_LED_GPIO        GPIO_NUM_41   // red LED D1 via R16, high = on (no controller)
#define BOARD_SD_PWR_GPIO     GPIO_NUM_42   // microSD power rail, unused
#define BOARD_KEY_MENU_GPIO   GPIO_NUM_2    // all keys active low, pull-ups on board
#define BOARD_KEY_EXIT_GPIO   GPIO_NUM_1
#define BOARD_KEY_UP_GPIO     GPIO_NUM_6
#define BOARD_KEY_DOWN_GPIO   GPIO_NUM_4
#define BOARD_KEY_OK_GPIO     GPIO_NUM_5
#define BOARD_KEY_BOOT_GPIO   GPIO_NUM_0
#define BOARD_HAS_EPD         1
#else
#error "no board selected"
#endif

#define BOARD_I2C_SDA_GPIO    ((gpio_num_t)CONFIG_HUB_I2C_SDA_GPIO)
#define BOARD_I2C_SCL_GPIO    ((gpio_num_t)CONFIG_HUB_I2C_SCL_GPIO)
#if CONFIG_HUB_I2C2_ENABLE
#define BOARD_I2C_BUSES       2
#else
#define BOARD_I2C_BUSES       1
#endif

// Pins of I2C bus 0 (IO3/IO8) or 1 (IO15/IO16).
void board_i2c_pins(int bus, gpio_num_t *sda, gpio_num_t *scl);

// Power rails, LED, I2C pull-up check. Call first.
void board_init(void);

// Level of SDA/SCL with internal pull-ups off, sampled in board_init() before the I2C
// driver takes the pins. 1/1 means external pull-ups are present.
void board_i2c_idle_levels(int bus, int *sda, int *scl);

// Starts the key scanner; posts APP_EVENT_KEY. Needs the default event loop.
void board_keys_start(void);

void board_led_set(bool on);

#ifdef __cplusplus
}
#endif
