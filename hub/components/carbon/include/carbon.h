// Carbon (Graphite plaintext protocol, TCP) sender and the self monitoring metrics.
//
// At every boundary of the Carbon interval the Carbon window of all metrics is closed and one
// batch of lines is buffered (PSRAM), time stamp = the boundary (UTC):
//   <prefix>.<hostname>.<instance>.<quantity>       mean over the interval (state: last value)
//   <prefix>.<hostname>.<instance>.<quantity>_min   only if the interval held more than one
//   <prefix>.<hostname>.<instance>.<quantity>_max   reading of a gauge
// Batches are sent once the clock is set (NTP); while the receiver is unreachable they stay in
// the buffer (oldest dropped when full) and are sent later with their original time stamps.
//
// Self monitoring ("hub.*"): uptime, reset reason, free heap, RSSI, reconnects, NTP offset,
// send errors of Carbon and syslog, read errors per sensor - published at every sensor
// interval boundary, so they show in "show status" and go to Carbon like any other metric.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t carbon_start(void);
// intervals or target changed: plan again
void carbon_reschedule(void);

typedef struct {
    bool enabled;              // host configured and primary hub
    bool secondary;            // role secondary: Carbon is off on this hub
    bool waiting_clock;
    uint32_t buffered_batches;
    uint32_t buffered_bytes;
    uint32_t sent_batches;
    uint32_t sent_lines;
    uint32_t dropped_batches;  // buffer full
    uint32_t errors;
    int64_t last_ok_us;        // esp_timer time of the last successful send, 0 = never
    char last_error[48];
    char peer[48];             // address of the last connection
} carbon_status_t;

void carbon_get_status(carbon_status_t *out);

#ifdef __cplusplus
}
#endif
