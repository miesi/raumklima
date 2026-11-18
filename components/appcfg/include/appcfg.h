// Device settings: defaults at build time, overrides in NVS, set on the console.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APPCFG_TZ_DEFAULT "CET-1CEST,M3.5.0,M10.5.0/3"
#define APPCFG_NTP_MAX 2
#define APPCFG_INTERVAL_MIN 10      // s, display and Carbon
#define APPCFG_INTERVAL_MAX 3600
#define APPCFG_INTERVAL_DEFAULT 60
#define APPCFG_CARBON_PORT 2003     // Graphite plaintext protocol
#define APPCFG_CARBON_PREFIX "raumklima"
#define APPCFG_SYSLOG_PORT 514

// syslog severities (RFC 5424) used as the level threshold
#define APPCFG_SEV_ERROR 3
#define APPCFG_SEV_WARNING 4
#define APPCFG_SEV_INFO 6
#define APPCFG_SEV_DEBUG 7

typedef struct {
    char hostname[33];          // DHCP option 12, max 32 chars, [a-z0-9-]
    char wifi_ssid[33];
    char wifi_psk[65];          // WPA2/WPA3 personal: 8..63 chars passphrase or 64 hex
    char tz[64];                // POSIX TZ string, display only; timestamps off-device are UTC
    char ntp[APPCFG_NTP_MAX][64]; // empty = DHCP option 42 with built-in fallback
    uint16_t display_s;         // display refresh interval
    uint16_t carbon_s;          // Carbon send interval
    char carbon_host[64];       // empty = Carbon off; name or address literal
    uint16_t carbon_port;
    char carbon_prefix[48];     // path: <prefix>.<hostname>.<instance>.<quantity>
    bool prefer_v4;             // Carbon and syslog: default IPv6 first, IPv4 second
    char syslog_host[64];       // empty = syslog off
    uint16_t syslog_port;
    bool syslog_tcp;            // false = UDP (RFC 5426), true = TCP (RFC 6587 octet counting)
    uint8_t syslog_level;       // APPCFG_SEV_*: send this severity and more severe ones
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
// APPCFG_INTERVAL_MIN..MAX; 0 = keep the current value
esp_err_t appcfg_set_intervals(int display_s, int carbon_s);
// host NULL or "" = off; port 0 = default; prefix NULL = keep
esp_err_t appcfg_set_carbon(const char *host, int port, const char *prefix);
void appcfg_set_prefer_v4(bool v4);
esp_err_t appcfg_set_syslog(const char *host, int port, bool tcp);
esp_err_t appcfg_set_syslog_level(int severity);

bool appcfg_dirty(void);
esp_err_t appcfg_save(void);

// Erases the whole NVS partition (settings, BSEC state). Caller reboots.
esp_err_t appcfg_factory_reset(void);

// True when WLAN credentials came from wlan.config at build time and NVS has none.
bool appcfg_wifi_from_build(void);

#ifdef __cplusplus
}
#endif
