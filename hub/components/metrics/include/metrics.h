// Latest value per metric plus aggregation windows. Producers: sensor drivers and the self
// monitoring. Consumers: UI, console, Carbon.
// Key: "<instance>.<quantity>_<unit>", e.g. "bme680.temperature_c".
//
// Windows: every reading of a gauge also goes into one open window per consumer (display, KNX,
// Carbon). The consumer closes its window at its interval boundary; the closed window holds
// count, mean, min and max of the readings since the previous boundary. State values (an
// accuracy, a status, a setting) are not averaged - the consumer uses the last value.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#define METRIC_KEY_LEN 40
#define METRICS_MAX CONFIG_HUB_METRICS_MAX

typedef enum {
    METRICS_WIN_DISPLAY = 0,
    METRICS_WIN_CARBON,
    METRICS_WIN_KNX,
    METRICS_WIN_COUNT,
} metrics_win_id_t;

typedef struct {
    uint32_t n;        // readings in the window, 0 = none
    float mean, min, max;
} metric_win_t;

typedef struct {
    char key[METRIC_KEY_LEN];
    float value;       // last reading
    int64_t mono_us;   // esp_timer time of the last reading
    time_t utc;        // 0 if the clock was not set yet
    bool state;        // state value: not averaged, consumers use the last value
    metric_win_t win[METRICS_WIN_COUNT];   // last closed window per consumer
} metric_t;

void metrics_init(void);

// Table full: a new key takes the slot of the oldest stale metric; without one it is dropped
// (error in the log, metrics_overflow() reports it).
void metrics_publish(const char *key, float value);         // gauge: averaged per window
void metrics_publish_state(const char *key, float value);   // state: last value only

// True if a new key was dropped in the last stale period; key = the last one dropped.
bool metrics_overflow(char *key, size_t len);

// A metric is stale (not shown, not sent) when its last reading is older than this. Set from
// the longest interval in use, so a sensor read once per interval does not flicker to "--".
void metrics_set_stale_us(int64_t us);
int64_t metrics_stale_us(void);
bool metrics_is_fresh(const metric_t *m);

// Closes the open window of a consumer: every metric's win[id] gets the readings since the
// previous close, the open window starts empty.
void metrics_close_window(metrics_win_id_t id);

// Value to show: the mean of the last closed display window, else the last reading.
float metrics_display_value(const metric_t *m);

// Exact key. False if missing or stale.
bool metrics_get(const char *key, metric_t *out);

// Best fresh value for a quantity across instances, e.g. "temperature_c".
// Priority by instance prefix: sht4x > bme6xx > scd4x > others.
bool metrics_get_primary(const char *quantity, metric_t *out);

// Like metrics_get_primary(), but only from sensors that measure the room air for its own sake:
// skips by-products such as the SCD4x temperature and humidity. For the overview tiles.
bool metrics_get_ambient(const char *quantity, metric_t *out);

// Calls fn for every metric (also stale ones), sorted by key.
void metrics_foreach(void (*fn)(const metric_t *m, void *arg), void *arg);

#ifdef __cplusplus
}
#endif
