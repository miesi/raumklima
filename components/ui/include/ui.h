// E-paper UI: LVGL 9, 1 bpp, full-frame render, pages switched with the rotary switch.
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Needs epd_init(), the default event loop and the data components running.
esp_err_t ui_start(void);

#ifdef __cplusplus
}
#endif
