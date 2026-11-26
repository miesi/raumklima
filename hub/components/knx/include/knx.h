// KNXnet/IP routing (multicast 224.0.23.12, UDP 3671).
//
// Send (primary hub only): at each boundary of the KNX interval (240..3600 s, aligned like the
// other intervals) the hub sends the mean of the interval of each "send" group address as
// GroupValueWrite, DPT 9. It also answers GroupValueRead for these addresses. The telegrams
// leave one at a time with a gap of KNX_TX_GAP_MS, so that the KNX IP router does not put a
// burst onto the 9600 bit/s TP line; ROUTING_BUSY from a router stops the sending for the
// time it asks for.
// Listen (all hubs): "listen" group addresses keep the last value and the time of the last
// telegram, for the KNX page. Received values go nowhere else (not to Carbon, not to KNX).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"
#include "metrics.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KNX_GA_MAX 64
#define KNX_NAME_LEN METRIC_KEY_LEN
#define KNX_TX_GAP_MS 200   // at most 5 telegrams/s from the hub

typedef enum {
    KNX_SEND,
    KNX_LISTEN,
} knx_dir_t;

typedef struct {
    uint16_t ga;
    uint16_t dpt;                 // DPT 9 subtype: 1 = 9.001
    knx_dir_t dir;
    char name[KNX_NAME_LEN];      // send: metric key; listen: optional name for the display
    // run time
    uint32_t telegrams;           // received for this address (write, response, read)
    int64_t last_rx_us;           // esp_timer time of the last received telegram, 0 = none
    uint16_t last_src;
    bool has_value;               // listen: value of the last write or response
    double value;
    bool sent;                    // send: a value went out
    double sent_value;
    int64_t sent_us;
} knx_ga_info_t;

typedef struct {
    bool on, running, primary;
    uint16_t ia;
    int interval_s;
    uint32_t rx;                  // telegrams received (all, own ones not counted)
    uint32_t rx_last_min;         // telegrams in the last full minute: the bus traffic
    uint32_t tx_write, tx_response, tx_dropped, busy;
    int64_t last_rx_us;
    int queued;
    char error[48];               // last socket error, "" if none
} knx_status_t;

// After netmgr_start(). Loads the group addresses from NVS and starts the task.
esp_err_t knx_start(void);

// Adds or replaces a group address; written to NVS at once. Checks that a send address has a
// DPT that fits the unit of its metric (temperature _c = 9.001, ...). msg: why it was refused
// or a note, "" if none.
esp_err_t knx_ga_add(uint16_t ga, uint16_t dpt, knx_dir_t dir, const char *name, char *msg, size_t msg_len);
esp_err_t knx_ga_del(uint16_t ga);
int knx_ga_list(knx_ga_info_t *out, int max);

void knx_get_status(knx_status_t *out);

// Unit of a DPT 9 subtype for the display, "" if unknown. 9.006 (Pa) is shown as hPa:
// knx_display_value() converts.
const char *knx_dpt_unit(uint16_t dpt);
double knx_display_value(uint16_t dpt, double v);

// Settings changed (interval, role, address, on/off): plan again.
void knx_reschedule(void);

// Test: send all telegrams unicast to this IPv4 address (network byte order) instead of the
// multicast group, 0 = back to multicast. RAM only: a reboot sends to the group again. With a
// test target nothing reaches the KNX line.
void knx_set_test_target(uint32_t ip);
uint32_t knx_test_target(void);

// Log each received telegram for the next seconds (event=rx), 0 = stop. Sends nothing.
void knx_monitor(int seconds);

// SEARCH_REQUEST (IP side only). The answers arrive in the next seconds.
void knx_search_start(void);
int knx_search_results(char (*lines)[128], int max);

#ifdef __cplusplus
}
#endif
