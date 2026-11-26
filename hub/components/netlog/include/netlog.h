// Syslog client (RFC 5424) for every ESP_LOG line, plain UDP (RFC 5426) or TCP with octet
// counting (RFC 6587). The console output goes through the same hook.
//
// Every line carries its subsystem - the ESP_LOG tag: a component ("timesync", "carbon") or a
// sensor instance ("sht4x", "scd41") - as syslog MSGID and at the start of the message, like the
// queue ID in a Postfix log line. The rest is key=value pairs; a line written as free text is
// sent as msg="...":
//   <134>1 2026-11-18T09:12:03.123Z hub-e1a418 raumklima-hub - sht4x - sht4x: event=read_failed err=ESP_ERR_INVALID_CRC
//
// Lines are buffered (PSRAM ring) from boot on and sent once the clock is set (NTP) and a
// syslog host is configured; buffered lines get their real time stamp.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_log.h"

#ifdef __cplusplus
extern "C" {
#endif

// Installs the log hook and the buffer. Call first in app_main.
esp_err_t netlog_init(void);
// Applies the log levels from appcfg (syslog level, console level). After appcfg_init() and
// after every "set syslog".
void netlog_apply(void);
// Starts the sender task. After netmgr_start().
esp_err_t netlog_start(void);

// Console output threshold: ESP_LOG_INFO ("log on") or ESP_LOG_WARN ("log off").
void netlog_set_console_level(esp_log_level_t level);

typedef struct {
    bool enabled;          // host configured
    bool waiting_clock;    // enabled, but no NTP time yet
    uint32_t buffered;     // lines waiting
    uint32_t sent;
    uint32_t dropped;      // buffer full, oldest lines lost
    uint32_t errors;       // send / connect failures
    char last_error[48];
} netlog_status_t;

void netlog_get_status(netlog_status_t *out);

#ifdef __cplusplus
}
#endif
