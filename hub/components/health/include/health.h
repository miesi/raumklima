// Crash and hang handling.
//
// Hangs: every long-running task registers once and calls health_alive() in each pass of its
// loop with the interval it expects to come back in. A supervisor task checks every second;
// a task that is overdue by more than 2 x interval + 30 s is taken as hung and the supervisor
// aborts (esp_system_abort): the panic handler writes a core dump with the stacks of all tasks,
// so the dump shows where the hung task waits. The supervisor itself is watched by the task
// watchdog. Busy loops and blocked interrupts are caught by the task and interrupt watchdogs.
//
// Crashes: the uptime of each run is kept in RTC memory (no flash writes; lost on power-off).
// After a crash reset the crashed run is added to a history of the last three; three crashes
// within 10 minutes of run time start the next boot in safe mode (no sensors, no display) so
// the core dump can be fetched. A reset that is not a crash (reboot command, power-on) clears
// the history.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_system.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HEALTH_MAX_WATCHED 8
#define HEALTH_SAFE_CRASHES 3
#define HEALTH_SAFE_WINDOW_S 600

typedef int health_id_t;   // -1 = not registered (table full), health_alive() ignores it

// First thing after netlog_init(): reads the reset reason and the RTC history, decides about
// safe mode and logs a core dump summary if one is in flash.
void health_init(void);

// True if this boot runs in safe mode.
bool health_safe_mode(void);

// Starts the supervisor task.
esp_err_t health_start(void);

// Registers a task to watch; the first health_alive() starts the watch.
health_id_t health_register(const char *name);

// Called in each pass of the task's loop. interval_us: the longest regular wait until the next
// pass (a sleep, a queue timeout).
void health_alive(health_id_t id, int64_t interval_us);

typedef struct {
    char name[16];
    bool started;
    int64_t age_us;        // since the last health_alive()
    int64_t allowed_us;    // overdue limit: 2 x interval + 30 s
} health_watch_t;

typedef struct {
    bool safe_mode;
    esp_reset_reason_t reset_reason;
    uint32_t boots;                       // since power-on
    uint32_t crashes;                     // since power-on
    int history_n;                        // crashes in the history (runs that ended in a crash)
    uint32_t history_s[HEALTH_SAFE_CRASHES];   // their run times, newest first
    char last_stall[16];                  // task the supervisor aborted for, "" if none
    bool coredump;                        // a valid core dump is in flash
    esp_err_t coredump_err;               // ESP_OK, ESP_ERR_NOT_FOUND or the check error
    int n_watch;
    health_watch_t watch[HEALTH_MAX_WATCHED];
} health_status_t;

void health_get_status(health_status_t *out);

// One-line summary of the core dump in flash (key=value), e.g. for the console.
// ESP_ERR_NOT_FOUND if there is none.
esp_err_t health_coredump_summary(char *buf, size_t len);

// Erases the core dump in flash (after it was fetched) and posts APP_EVENT_COREDUMP_ERASED.
esp_err_t health_coredump_erase(void);

// While a valid core dump is in flash: true and the download token (HEALTH_TOKEN_LEN hex
// characters). The token is random for each boot (hardware RNG before the radio starts).
#define HEALTH_TOKEN_LEN 16
bool health_coredump_token(char *buf, size_t len);

// Test helpers for the console: "panic", "fault", "irqoff" do not return; "spin" starts a
// task pinned to core 1 that never yields and returns.
void health_crash(const char *how);

#ifdef __cplusplus
}
#endif
