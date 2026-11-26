// Init order only - no logic here.
#include "app_events.h"
#include "appcfg.h"
#include "board.h"
#include "carbon.h"
#include "console_cmds.h"
#include "epd.h"
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "health.h"
#include "i2cbus.h"
#include "metrics.h"
#include "netlog.h"
#include "netmgr.h"
#include "onewire.h"
#include "sensors.h"
#include "timesync.h"
#include "ui.h"

static const char *TAG = "main";

void app_main(void)
{
    netlog_init();                      // log hook first: boot lines reach syslog later
    ESP_LOGI(TAG, "event=boot app=%s version=%s reset_reason=%d", esp_app_get_description()->project_name,
             esp_app_get_description()->version, esp_reset_reason());
    health_init();                      // crash history, safe mode decision, core dump report
    ESP_ERROR_CHECK(health_start());    // supervisor: aborts with a core dump if a task hangs
    board_init();                       // rails, LED, pull-up check (before I2C takes the pins)
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(appcfg_init());     // NVS
    netlog_apply();                     // log levels from the settings
    metrics_init();
    ow_names_init();                    // 1-Wire probe names from NVS

    ESP_ERROR_CHECK(i2cbus_init());
    sensors_start();                    // safe mode: locks only, no probing, no task

    ESP_ERROR_CHECK(netmgr_start());
    timesync_start();
    netlog_start();                     // syslog sender
    carbon_start();                     // Carbon sender and self monitoring

    if (health_safe_mode()) {
        ESP_LOGW(TAG, "event=display_off reason=safe_mode");
    } else if (epd_init() == ESP_OK) {
        ui_start();
    } else {
        ESP_LOGE(TAG, "event=epd_init_failed display=none");
    }

    board_keys_start();
    console_cmds_start();
}
