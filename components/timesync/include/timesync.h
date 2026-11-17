// SNTP. Servers: console config > DHCP option 42 > built-in fallback.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Fallback without DHCP option 42: a DNS name, not an address, so the operator can move it.
// lwIP resolves it on every request, IPv4 first, IPv6 if there is no A record
// (LWIP_DNS_ADDRTYPE_DEFAULT). Needs a working DNS server from DHCP.
#define TIMESYNC_FALLBACK "ntp.1und1.de"
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

// Re-applies TZ and NTP servers from appcfg (after "set ntp"). Restarts SNTP: servers learned
// from DHCP option 42 are gone until the next DHCP ACK (lease renewal or reconnect).
esp_err_t timesync_apply(void);

// Re-applies only the TZ from appcfg (after "set tz"); SNTP keeps running.
void timesync_apply_tz(void);

void timesync_get_status(timesync_status_t *out);

const char *timesync_source_name(timesync_source_t s);

// ISO 8601 UTC offset of local time at t, e.g. "+01:00" (CET) or "+02:00" (CEST).
void timesync_iso_offset(time_t t, char *buf, size_t len);

#ifdef __cplusplus
}
#endif
