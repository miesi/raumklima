#include "board.h"

#include "app_events.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board";

static int s_sda_idle[BOARD_I2C_BUSES];
static int s_scl_idle[BOARD_I2C_BUSES];

void board_i2c_pins(int bus, gpio_num_t *sda, gpio_num_t *scl)
{
#if CONFIG_HUB_I2C2_ENABLE
    if (bus == 1) {
        *sda = (gpio_num_t)CONFIG_HUB_I2C2_SDA_GPIO;
        *scl = (gpio_num_t)CONFIG_HUB_I2C2_SCL_GPIO;
        return;
    }
#endif
    *sda = BOARD_I2C_SDA_GPIO;
    *scl = BOARD_I2C_SCL_GPIO;
}

static void probe_pullups(void)
{
    // Discharge the line (drive it low), release it with internal pulls off and read after
    // 200 us: an external pull-up (kOhm) has pulled it high by then, an open pin stays low.
    // (gpio_reset_pin() enables the internal pull-up, so without the discharge an unconnected
    // pin keeps reading 1.)
    for (int b = 0; b < BOARD_I2C_BUSES; b++) {
        gpio_num_t pins[2];
        board_i2c_pins(b, &pins[0], &pins[1]);
        for (int i = 0; i < 2; i++) {
            gpio_reset_pin(pins[i]);
            gpio_set_pull_mode(pins[i], GPIO_FLOATING);
            gpio_set_direction(pins[i], GPIO_MODE_OUTPUT);
            gpio_set_level(pins[i], 0);
        }
        esp_rom_delay_us(10);
        for (int i = 0; i < 2; i++) {
            gpio_set_direction(pins[i], GPIO_MODE_INPUT);
        }
        esp_rom_delay_us(200);
        s_sda_idle[b] = gpio_get_level(pins[0]);
        s_scl_idle[b] = gpio_get_level(pins[1]);
        ESP_LOGI(TAG, "event=i2c_idle_levels bus=%d sda_gpio=%d sda=%d scl_gpio=%d scl=%d pullups=%s", b + 1, pins[0],
                 s_sda_idle[b], pins[1], s_scl_idle[b], (s_sda_idle[b] && s_scl_idle[b]) ? "present" : "missing");
    }
}

void board_init(void)
{
    const gpio_config_t out = {
        .pin_bit_mask = (1ULL << BOARD_EPD_PWR_GPIO) | (1ULL << BOARD_LED_GPIO) | (1ULL << BOARD_SD_PWR_GPIO),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out);
    gpio_set_level(BOARD_SD_PWR_GPIO, 0);   // microSD not used
    gpio_set_level(BOARD_EPD_PWR_GPIO, 1);  // panel power on, the EPD driver handles sleep
    gpio_set_level(BOARD_LED_GPIO, 0);   // red LED D1: off by default, 'led on|off' on the console
    probe_pullups();
}

void board_i2c_idle_levels(int bus, int *sda, int *scl)
{
    *sda = bus < BOARD_I2C_BUSES ? s_sda_idle[bus] : -1;
    *scl = bus < BOARD_I2C_BUSES ? s_scl_idle[bus] : -1;
}

void board_led_set(bool on)
{
    gpio_set_level(BOARD_LED_GPIO, on ? 1 : 0);
}

// --- keys: 10 ms polling, 30 ms debounce, long press >= 1 s ---

typedef struct {
    gpio_num_t gpio;
    app_key_t key;
    uint8_t stable;      // debounced state, 1 = pressed
    uint8_t count;       // consecutive samples differing from stable
    uint16_t held_ms;
    bool long_sent;
} key_state_t;

static key_state_t s_keys[] = {
    {.gpio = BOARD_KEY_MENU_GPIO, .key = APP_KEY_MENU},
    {.gpio = BOARD_KEY_EXIT_GPIO, .key = APP_KEY_EXIT},
    {.gpio = BOARD_KEY_UP_GPIO, .key = APP_KEY_UP},
    {.gpio = BOARD_KEY_DOWN_GPIO, .key = APP_KEY_DOWN},
    {.gpio = BOARD_KEY_OK_GPIO, .key = APP_KEY_OK},
    {.gpio = BOARD_KEY_BOOT_GPIO, .key = APP_KEY_BOOT},
};

#define KEY_POLL_MS     10
#define KEY_DEBOUNCE_N  3
#define KEY_LONG_MS     1000

static void post_key(app_key_t key, bool long_press)
{
    app_key_event_t ev = {.key = key, .long_press = long_press};
    esp_event_post(APP_EVENT, APP_EVENT_KEY, &ev, sizeof(ev), 0);
}

static void keys_task(void *arg)
{
    for (;;) {
        for (size_t i = 0; i < sizeof(s_keys) / sizeof(s_keys[0]); i++) {
            key_state_t *k = &s_keys[i];
            uint8_t pressed = gpio_get_level(k->gpio) == 0;
            if (pressed != k->stable) {
                if (++k->count >= KEY_DEBOUNCE_N) {
                    k->stable = pressed;
                    k->count = 0;
                    if (pressed) {
                        k->held_ms = 0;
                        k->long_sent = false;
                    } else if (!k->long_sent) {
                        post_key(k->key, false);   // short press fires on release
                    }
                }
            } else {
                k->count = 0;
                if (k->stable && !k->long_sent) {
                    k->held_ms += KEY_POLL_MS;
                    if (k->held_ms >= KEY_LONG_MS) {
                        k->long_sent = true;
                        post_key(k->key, true);
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(KEY_POLL_MS));
    }
}

void board_keys_start(void)
{
    uint64_t mask = 0;
    for (size_t i = 0; i < sizeof(s_keys) / sizeof(s_keys[0]); i++) {
        mask |= 1ULL << s_keys[i].gpio;
    }
    const gpio_config_t in = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,   // board has pull-ups too; internal ones do no harm
    };
    gpio_config(&in);
    xTaskCreate(keys_task, "keys", 2560, NULL, 5, NULL);
}
