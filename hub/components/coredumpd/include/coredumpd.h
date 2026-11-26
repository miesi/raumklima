// Core dump download over HTTP (port 80), only while a core dump is in flash.
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// After netmgr_start(). Without a core dump in flash it does nothing.
esp_err_t coredumpd_start(void);

#ifdef __cplusplus
}
#endif
