// Latest value per metric. Producers: sensor drivers. Consumers: UI, console, later Carbon.
// Key: "<instance>.<quantity>_<unit>", e.g. "bme680.temperature_c".
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define METRIC_KEY_LEN 40
#define METRICS_MAX 48
#define METRIC_STALE_US (60LL * 1000 * 1000)   // older than this = not shown

typedef struct {
    char key[METRIC_KEY_LEN];
    float value;
    int64_t mono_us;   // esp_timer time of the reading
    time_t utc;        // 0 if the clock was not set yet
} metric_t;

void metrics_init(void);

void metrics_publish(const char *key, float value);

// Exact key. False if missing or stale.
bool metrics_get(const char *key, metric_t *out);

// Best fresh value for a quantity across instances, e.g. "temperature_c".
// Priority by instance prefix: sht4x > bme6xx > scd4x > others.
bool metrics_get_primary(const char *quantity, metric_t *out);

// Calls fn for every metric (also stale ones), sorted by key.
void metrics_foreach(void (*fn)(const metric_t *m, void *arg), void *arg);

#ifdef __cplusplus
}
#endif
