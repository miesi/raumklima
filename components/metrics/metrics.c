#include "metrics.h"

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static metric_t s_m[METRICS_MAX];
static int s_n;
static SemaphoreHandle_t s_lock;

void metrics_init(void)
{
    s_lock = xSemaphoreCreateMutex();
}

static time_t utc_now(void)
{
    time_t t = time(NULL);
    return t > 1700000000 ? t : 0;   // before 2023-11 = clock not set
}

void metrics_publish(const char *key, float value)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int i;
    for (i = 0; i < s_n; i++) {
        if (strcmp(s_m[i].key, key) == 0) {
            break;
        }
    }
    if (i == s_n) {
        if (s_n == METRICS_MAX) {
            xSemaphoreGive(s_lock);
            return;
        }
        strlcpy(s_m[s_n].key, key, METRIC_KEY_LEN);
        s_n++;
    }
    s_m[i].value = value;
    s_m[i].mono_us = esp_timer_get_time();
    s_m[i].utc = utc_now();
    xSemaphoreGive(s_lock);
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

bool metrics_get_primary(const char *quantity, metric_t *out)
{
    int best = -1, best_prio = 99;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_n; i++) {
        const char *dot = strchr(s_m[i].key, '.');
        if (!dot || strcmp(dot + 1, quantity) != 0 || !fresh(&s_m[i])) {
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
