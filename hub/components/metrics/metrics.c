#include "metrics.h"

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "metrics";

typedef struct {
    double sum;
    float min, max;
    uint32_t n;
} acc_t;

static metric_t s_m[METRICS_MAX];
static acc_t s_acc[METRICS_MAX][METRICS_WIN_COUNT];   // open windows
static int s_n;
static int64_t s_stale_us = 60LL * 1000 * 1000;
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
        if (now - s_m[i].mono_us >= s_stale_us && (best < 0 || s_m[i].mono_us < s_m[best].mono_us)) {
            best = i;
        }
    }
    return best;
}

static void publish(const char *key, float value, bool state)
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
            ESP_LOGW(TAG, "event=table_full max=%d key=%s replaces_stale=%s", METRICS_MAX, key, s_m[i].key);
        } else {
            // the metrics that are there keep working; say so once per episode, not per reading
            if (s_dropped_us == 0 || now - s_dropped_us > s_stale_us) {
                ESP_LOGE(TAG, "event=table_full max=%d dropped=%s hint=\"raise CONFIG_HUB_METRICS_MAX\"",
                         METRICS_MAX, key);
            }
            s_dropped_us = now;
            strlcpy(s_dropped_key, key, sizeof(s_dropped_key));
            xSemaphoreGive(s_lock);
            return;
        }
        memset(&s_m[i], 0, sizeof(s_m[i]));
        memset(s_acc[i], 0, sizeof(s_acc[i]));
        strlcpy(s_m[i].key, key, METRIC_KEY_LEN);
    }
    s_m[i].value = value;
    s_m[i].mono_us = now;
    s_m[i].utc = utc_now();
    s_m[i].state = state;
    for (int w = 0; w < METRICS_WIN_COUNT; w++) {
        acc_t *a = &s_acc[i][w];
        if (a->n == 0 || value < a->min) {
            a->min = value;
        }
        if (a->n == 0 || value > a->max) {
            a->max = value;
        }
        a->sum += value;
        a->n++;
    }
    xSemaphoreGive(s_lock);
}

void metrics_publish(const char *key, float value)
{
    publish(key, value, false);
}

void metrics_publish_state(const char *key, float value)
{
    publish(key, value, true);
}

void metrics_set_stale_us(int64_t us)
{
    s_stale_us = us;
}

int64_t metrics_stale_us(void)
{
    return s_stale_us;
}

void metrics_close_window(metrics_win_id_t id)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_n; i++) {
        acc_t *a = &s_acc[i][id];
        metric_win_t *w = &s_m[i].win[id];
        w->n = a->n;
        w->mean = a->n ? (float)(a->sum / a->n) : 0;
        w->min = a->min;
        w->max = a->max;
        memset(a, 0, sizeof(*a));
    }
    xSemaphoreGive(s_lock);
}

float metrics_display_value(const metric_t *m)
{
    const metric_win_t *w = &m->win[METRICS_WIN_DISPLAY];
    return (!m->state && w->n > 0) ? w->mean : m->value;
}

bool metrics_overflow(char *key, size_t len)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool recent = s_dropped_us != 0 && esp_timer_get_time() - s_dropped_us <= s_stale_us;
    if (recent) {
        strlcpy(key, s_dropped_key, len);
    }
    xSemaphoreGive(s_lock);
    return recent;
}

bool metrics_is_fresh(const metric_t *m)
{
    return esp_timer_get_time() - m->mono_us < s_stale_us;
}

static bool fresh(const metric_t *m)
{
    return metrics_is_fresh(m);
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
