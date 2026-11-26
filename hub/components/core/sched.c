#include "sched.h"

#include <sys/time.h>

#include "esp_timer.h"

#define CLOCK_VALID_AFTER 1700000000   // 2023-11: anything before is the unset clock

bool sched_clock_valid(void)
{
    return time(NULL) > CLOCK_VALID_AFTER;
}

// wall clock (us) and esp_timer (us) read as close together as possible
static void now_pair(int64_t *wall, int64_t *mono)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    *mono = esp_timer_get_time();
    *wall = (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

static int64_t floor_div(int64_t a, int64_t b)
{
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

int64_t sched_next_us(int interval_s, int64_t after_us)
{
    int64_t iv = (int64_t)interval_s * 1000000;
    if (!sched_clock_valid()) {
        return (floor_div(after_us, iv) + 1) * iv;
    }
    int64_t wall, mono;
    now_pair(&wall, &mono);
    int64_t w = wall + (after_us - mono);
    return mono + ((floor_div(w, iv) + 1) * iv - wall);
}

int64_t sched_prev_us(int interval_s, int64_t at_us)
{
    int64_t iv = (int64_t)interval_s * 1000000;
    if (!sched_clock_valid()) {
        return floor_div(at_us, iv) * iv;
    }
    int64_t wall, mono;
    now_pair(&wall, &mono);
    int64_t w = wall + (at_us - mono);
    return mono + (floor_div(w, iv) * iv - wall);
}

time_t sched_utc(int64_t mono_us)
{
    if (!sched_clock_valid()) {
        return 0;
    }
    int64_t wall, mono;
    now_pair(&wall, &mono);
    int64_t w = wall + (mono_us - mono);
    return (time_t)floor_div(w + 500000, 1000000);   // rounded to the second
}
