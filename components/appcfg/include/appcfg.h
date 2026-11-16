// Device settings: defaults at build time, overrides in NVS, set on the console.
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APPCFG_NTP_MAX 2

typedef struct {
    char hostname[33];          // DHCP option 12, max 32 chars, [a-z0-9-]
    char wifi_ssid[33];
    char wifi_psk[65];          // WPA2/WPA3 personal: 8..63 chars passphrase or 64 hex
    char tz[64];                // POSIX TZ string, display only; timestamps off-device are UTC
    char ntp[APPCFG_NTP_MAX][64]; // empty = DHCP option 42 with built-in fallback
} appcfg_t;

// Initialises NVS and loads settings. Call once, early.
esp_err_t appcfg_init(void);

// Thread-safe copy of the current (possibly unsaved) settings.
void appcfg_get(appcfg_t *out);

// Setters validate and change the running config; appcfg_save() makes them persistent.
esp_err_t appcfg_set_hostname(const char *name);
esp_err_t appcfg_set_wifi(const char *ssid, const char *psk);
esp_err_t appcfg_set_tz(const char *tz);
esp_err_t appcfg_set_ntp(const char *s1, const char *s2);   // both NULL = back to DHCP/fallback

bool appcfg_dirty(void);
esp_err_t appcfg_save(void);

// Erases the whole NVS partition (settings, BSEC state). Caller reboots.
esp_err_t appcfg_factory_reset(void);

// True when WLAN credentials came from wlan.config at build time and NVS has none.
bool appcfg_wifi_from_build(void);

#ifdef __cplusplus
}
#endif
