// Latest value per metric. Producers: sensor drivers. Consumers: UI, console, later Carbon.
// Key: "<instance>.<quantity>_<unit>", e.g. "bme680.temperature_c".
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
#define METRIC_STALE_US (60LL * 1000 * 1000)   // older than this = not shown

typedef struct {
    char key[METRIC_KEY_LEN];
    float value;
    int64_t mono_us;   // esp_timer time of the reading
    time_t utc;        // 0 if the clock was not set yet
} metric_t;

void metrics_init(void);

// Table full: a new key takes the slot of the oldest stale metric; without one it is dropped
// (error in the log, metrics_overflow() reports it).
void metrics_publish(const char *key, float value);

// True if a new key was dropped in the last METRIC_STALE_US; key = the last one dropped.
bool metrics_overflow(char *key, size_t len);

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
