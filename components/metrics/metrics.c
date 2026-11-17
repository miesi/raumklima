#include "metrics.h"

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "metrics";

static metric_t s_m[METRICS_MAX];
static int s_n;
static SemaphoreHandle_t s_lock;
static int64_t s_dropped_us;            // last time a new key found no slot, 0 = never
static char s_dropped_key[METRIC_KEY_LEN];

void metrics_init(void)
{
    s_lock = xSemaphoreCreateMutex();
}

static time_t utc_now(void)
{
    time_t t = time(NULL);
    return t > 1700000000 ? t : 0;   // before 2023-11 = clock not set
}

// Slot for a new key in a full table: the oldest stale metric (a renamed or removed probe,
// a pulled sensor), -1 if all are fresh.
static int stale_slot(int64_t now)
{
    int best = -1;
    for (int i = 0; i < s_n; i++) {
        if (now - s_m[i].mono_us >= METRIC_STALE_US && (best < 0 || s_m[i].mono_us < s_m[best].mono_us)) {
            best = i;
        }
    }
    return best;
}

void metrics_publish(const char *key, float value)
{
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int i;
    for (i = 0; i < s_n; i++) {
        if (strcmp(s_m[i].key, key) == 0) {
            break;
        }
    }
    if (i == s_n) {
        if (s_n < METRICS_MAX) {
            s_n++;
        } else if ((i = stale_slot(now)) >= 0) {
            ESP_LOGW(TAG, "table full (%d): %s replaces stale %s", METRICS_MAX, key, s_m[i].key);
        } else {
            // the metrics that are there keep working; say so once per episode, not per reading
            if (s_dropped_us == 0 || now - s_dropped_us > METRIC_STALE_US) {
                ESP_LOGE(TAG, "table full (%d fresh metrics), dropping %s - raise CONFIG_HUB_METRICS_MAX",
                         METRICS_MAX, key);
            }
            s_dropped_us = now;
            strlcpy(s_dropped_key, key, sizeof(s_dropped_key));
            xSemaphoreGive(s_lock);
            return;
        }
        strlcpy(s_m[i].key, key, METRIC_KEY_LEN);
    }
    s_m[i].value = value;
    s_m[i].mono_us = now;
    s_m[i].utc = utc_now();
    xSemaphoreGive(s_lock);
}

bool metrics_overflow(char *key, size_t len)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool recent = s_dropped_us != 0 && esp_timer_get_time() - s_dropped_us <= METRIC_STALE_US;
    if (recent) {
        strlcpy(key, s_dropped_key, len);
    }
    xSemaphoreGive(s_lock);
    return recent;
}

static bool fresh(const metric_t *m)
{
    return esp_timer_get_time() - m->mono_us < METRIC_STALE_US;
}

bool metrics_get(const char *key, metric_t *out)
{
    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_n; i++) {
        if (strcmp(s_m[i].key, key) == 0) {
            *out = s_m[i];
            ok = fresh(&s_m[i]);
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return ok;
}

static int prio(const char *key)
{
    static const char *order[] = {"sht4", "bme6", "scd4"};
    for (int i = 0; i < 3; i++) {
        if (strncmp(key, order[i], strlen(order[i])) == 0) {
            return i;
        }
    }
    return 3;
}

// Instances whose temperature and humidity are a by-product of another measurement: the SCD4x
// measures them inside its own housing, next to its heated IR source, for its compensation.
static bool byproduct(const char *key)
{
    return strncmp(key, "scd4", 4) == 0;
}

static bool get_best(const char *quantity, bool ambient, metric_t *out)
{
    int best = -1, best_prio = 99;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_n; i++) {
        const char *dot = strchr(s_m[i].key, '.');
        if (!dot || strcmp(dot + 1, quantity) != 0 || !fresh(&s_m[i]) || (ambient && byproduct(s_m[i].key))) {
            continue;
        }
        int p = prio(s_m[i].key);
        if (p < best_prio) {
            best = i;
            best_prio = p;
        }
    }
    if (best >= 0) {
        *out = s_m[best];
    }
    xSemaphoreGive(s_lock);
    return best >= 0;
}

bool metrics_get_primary(const char *quantity, metric_t *out)
{
    return get_best(quantity, false, out);
}

bool metrics_get_ambient(const char *quantity, metric_t *out)
{
    return get_best(quantity, true, out);
}

static int cmp(const void *a, const void *b)
{
    return strcmp(((const metric_t *)a)->key, ((const metric_t *)b)->key);
}

void metrics_foreach(void (*fn)(const metric_t *m, void *arg), void *arg)
{
    metric_t *copy = malloc(sizeof(s_m));
    if (!copy) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_n;
    memcpy(copy, s_m, sizeof(metric_t) * n);
    xSemaphoreGive(s_lock);
    qsort(copy, n, sizeof(metric_t), cmp);
    for (int i = 0; i < n; i++) {
        fn(&copy[i], arg);
    }
    free(copy);
}
