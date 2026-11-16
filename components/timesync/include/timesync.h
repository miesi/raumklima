// SNTP. Servers: console config > DHCP option 42 > built-in fallback.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TIMESYNC_FALLBACK_V4 "212.227.123.5"
#define TIMESYNC_FALLBACK_V6 "2001:8d8:fe:123::5"
#define TIMESYNC_SLOTS 3

typedef enum {
    TIMESYNC_SRC_FALLBACK,  // no option 42 from DHCP, built-in servers
    TIMESYNC_SRC_DHCP,      // option 42
    TIMESYNC_SRC_CONFIG,    // set on the console
} timesync_source_t;

typedef struct {
    bool synced;                 // at least one sync since boot
    time_t last_sync;            // UTC
    uint32_t sync_count;
    timesync_source_t source;
    char server[TIMESYNC_SLOTS][48];   // "" = slot unused
    uint8_t reach[TIMESYNC_SLOTS];     // lwIP reachability shift register per slot
} timesync_status_t;

// Sets TZ, starts SNTP. Call after netmgr_start().
esp_err_t timesync_start(void);

// Re-applies TZ and NTP servers from appcfg (after "set tz", "set ntp").
esp_err_t timesync_apply(void);

void timesync_get_status(timesync_status_t *out);

const char *timesync_source_name(timesync_source_t s);

#ifdef __cplusplus
}
#endif
