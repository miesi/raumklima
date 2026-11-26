// Interval boundaries for the display, Carbon and the sensors without a cadence of their own.
// With a set clock (NTP) a boundary is a multiple of the interval in UTC seconds since the
// epoch: 60 s = at the start of every minute, 3600 s = at the full hour. Before the first
// sync the boundaries count from boot; after a clock step the next boundary follows the new
// clock.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

// The clock was set (NTP); before that, time() counts from 1970 at boot.
bool sched_clock_valid(void);

// esp_timer time (us) of the first boundary of interval_s strictly after esp_timer time after_us.
int64_t sched_next_us(int interval_s, int64_t after_us);

// esp_timer time (us) of the latest boundary at or before esp_timer time at_us.
int64_t sched_prev_us(int interval_s, int64_t at_us);

// UTC seconds of an esp_timer time; 0 while the clock is not valid.
time_t sched_utc(int64_t mono_us);

#ifdef __cplusplus
}
#endif
