// Init order only - no logic here.
#include "app_events.h"
#include "appcfg.h"
#include "board.h"
#include "console_cmds.h"
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_log.h"
#include "i2cbus.h"
#include "metrics.h"
#include "netmgr.h"
#include "sensors.h"
#include "timesync.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "%s %s", esp_app_get_description()->project_name, esp_app_get_description()->version);

    board_init();                       // rails, LED, pull-up check (before I2C takes the pins)
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(appcfg_init());     // NVS
    metrics_init();

    ESP_ERROR_CHECK(i2cbus_init());
    sensors_start();

    ESP_ERROR_CHECK(netmgr_start());
    timesync_start();

    board_keys_start();
    console_cmds_start();
}
