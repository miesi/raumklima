// Application-wide events. Components talk through these, not through each other's internals.
#pragma once

#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif

ESP_EVENT_DECLARE_BASE(APP_EVENT);

typedef enum {
    APP_EVENT_NET_CHANGED,      // WLAN connect/disconnect, new IPv4/IPv6 address
    APP_EVENT_TIME_SYNCED,      // SNTP sync done (every sync, not only the first)
    APP_EVENT_SENSORS_CHANGED,  // sensor list changed (probe, rescan)
    APP_EVENT_KEY,              // button press, data: app_key_event_t
    APP_EVENT_COREDUMP_ERASED,  // the core dump in flash was erased (the HTTP endpoint closes)
} app_event_id_t;

typedef enum {
    APP_KEY_MENU,
    APP_KEY_EXIT,
    APP_KEY_UP,       // rotary switch up
    APP_KEY_DOWN,     // rotary switch down
    APP_KEY_OK,       // rotary switch press
    APP_KEY_BOOT,
} app_key_t;

typedef struct {
    app_key_t key;
    bool long_press;
} app_key_event_t;

#ifdef __cplusplus
}
#endif
