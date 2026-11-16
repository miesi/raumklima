#include "console_cmds.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "appcfg.h"
#include "board.h"
#include "esp_app_desc.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2cbus.h"
#include "metrics.h"
#include "netmgr.h"
#include "onewire.h"
#include "sensors.h"
#include "timesync.h"

void console_fmt_uptime(char *buf, size_t len, int64_t us, bool with_seconds)
{
    int64_t s = us / 1000000;
    int d = s / 86400;
    int h = (s / 3600) % 24;
    int m = (s / 60) % 60;
    if (with_seconds) {
        snprintf(buf, len, "%dd %02d:%02d:%02d", d, h, m, (int)(s % 60));
    } else {
        snprintf(buf, len, "%dd %02d:%02d", d, h, m);
    }
}

static const char *reset_reason(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "external";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    default: return "other";
    }
}

static void print_metric(const metric_t *m, void *arg)
{
    int64_t age_s = (esp_timer_get_time() - m->mono_us) / 1000000;
    printf("  %-28s %12.2f   age %llds%s\n", m->key, m->value, (long long)age_s,
           age_s * 1000000 > METRIC_STALE_US ? " STALE" : "");
}

static void show_status(void)
{
    char buf[64];
    const esp_app_desc_t *app = esp_app_get_description();
    console_fmt_uptime(buf, sizeof(buf), esp_timer_get_time(), true);
    printf("firmware   %s %s (IDF %s)\n", app->project_name, app->version, app->idf_ver);
    printf("board      %s\n", BOARD_NAME);
    printf("uptime     %s, last reset: %s\n", buf, reset_reason());
    printf("heap       internal %u free, PSRAM %u free\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    netmgr_status_t n;
    netmgr_get_status(&n);
    printf("hostname   %s\n", n.hostname);
    printf("mac        " MACSTR "\n", MAC2STR(n.mac));
    if (n.associated) {
        printf("wlan       connected to '%s', RSSI %d dBm, channel %u, BSSID " MACSTR "\n", n.ssid, n.rssi,
               n.channel, MAC2STR(n.bssid));
    } else {
        printf("wlan       NOT connected (ssid '%s', last reason %u, reconnects %" PRIu32 ")\n", n.ssid,
               n.last_disconnect_reason, n.reconnects);
    }
    if (n.has_ip4) {
        printf("ipv4       " IPSTR "/" IPSTR " gw " IPSTR " dns " IPSTR " " IPSTR "\n", IP2STR(&n.ip4.ip),
               IP2STR(&n.ip4.netmask), IP2STR(&n.ip4.gw), IP2STR(&n.dns4[0]), IP2STR(&n.dns4[1]));
    } else {
        printf("ipv4       none\n");
    }
    for (int i = 0; i < n.n_ip6; i++) {
        printf("ipv6       " IPV6STR "\n", IPV62STR(n.ip6[i]));
    }
    if (n.has_ip6_ll) {
        printf("ipv6 ll    " IPV6STR "\n", IPV62STR(n.ip6_ll));
    }

    timesync_status_t t;
    timesync_get_status(&t);
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &tm);
    printf("time       %s%s\n", buf, t.synced ? "" : " (NOT synced)");
    printf("ntp        source %s, syncs %" PRIu32, timesync_source_name(t.source), t.sync_count);
    if (t.synced) {
        gmtime_r(&t.last_sync, &tm);
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", &tm);
        printf(", last %s", buf);
    }
    printf("\n");
    for (int i = 0; i < TIMESYNC_SLOTS; i++) {
        if (t.server[i][0]) {
            printf("  server %d  %-40s reach 0x%02x\n", i, t.server[i], t.reach[i]);
        }
    }

    int sda, scl;
    board_i2c_idle_levels(&sda, &scl);
    printf("i2c        SDA=GPIO%d SCL=GPIO%d, idle levels at boot SDA=%d SCL=%d\n", BOARD_I2C_SDA_GPIO,
           BOARD_I2C_SCL_GPIO, sda, scl);
    sensor_info_t si[8];
    int ns = sensors_list(si, 8);
    for (int i = 0; i < ns; i++) {
        printf("sensor     0x%02x %-8s as %-8s ok %" PRIu32 " err %" PRIu32 "  %s%s\n", si[i].addr, si[i].model,
               si[i].name, si[i].ok_count, si[i].err_count, si[i].supported ? "" : "NOT SUPPORTED ", si[i].info);
    }
    char warn[128];
    sensors_warnings(warn, sizeof(warn));
    if (warn[0]) {
        printf("WARNING    %s\n", warn);
    }
    printf("metrics\n");
    metrics_foreach(print_metric, NULL);
}

static void show_config(void)
{
    appcfg_t c;
    appcfg_get(&c);
    printf("hostname   %s\n", c.hostname);
    printf("wifi       ssid '%s', psk %s (%s)\n", c.wifi_ssid, c.wifi_psk[0] ? "********" : "(none)",
           appcfg_wifi_from_build() ? "from wlan.config" : "NVS");
    printf("tz         %s\n", c.tz);
    if (c.ntp[0][0]) {
        printf("ntp        %s %s\n", c.ntp[0], c.ntp[1]);
    } else {
        printf("ntp        dhcp (fallback %s %s)\n", TIMESYNC_FALLBACK_V4, TIMESYNC_FALLBACK_V6);
    }
    printf("%s\n", appcfg_dirty() ? "UNSAVED changes - 'save' to keep them" : "saved");
}

static int cmd_show(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "status") == 0) {
        show_status();
    } else if (strcmp(argv[1], "config") == 0) {
        show_config();
    } else {
        printf("usage: show status|config\n");
        return 1;
    }
    return 0;
}

static int report(esp_err_t err, const char *what)
{
    if (err != ESP_OK) {
        printf("%s: %s\n", what, esp_err_to_name(err));
        return 1;
    }
    printf("ok - 'save' to keep it\n");
    return 0;
}

static int cmd_set(int argc, char **argv)
{
    if (argc >= 4 && strcmp(argv[1], "wifi") == 0) {
        if (report(appcfg_set_wifi(argv[2], argv[3]), "set wifi (ssid 1-32, psk 8-64 chars)")) {
            return 1;
        }
        netmgr_restart();
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "hostname") == 0) {
        if (report(appcfg_set_hostname(argv[2]), "set hostname ([a-z0-9-], max 32)")) {
            return 1;
        }
        printf("reconnecting so DHCP sends the new name\n");
        netmgr_restart();
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "tz") == 0) {
        if (report(appcfg_set_tz(argv[2]), "set tz")) {
            return 1;
        }
        timesync_apply();
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "ntp") == 0) {
        esp_err_t err = strcmp(argv[2], "dhcp") == 0 ? appcfg_set_ntp(NULL, NULL)
                                                      : appcfg_set_ntp(argv[2], argc >= 4 ? argv[3] : NULL);
        if (report(err, "set ntp")) {
            return 1;
        }
        timesync_apply();
        return 0;
    }
    printf("usage:\n"
           "  set wifi <ssid> <psk>          WPA2/WPA3 personal\n"
           "  set hostname <name>            DHCP option 12\n"
           "  set tz <posix>                 e.g. UTC0 or CET-1CEST,M3.5.0,M10.5.0/3\n"
           "  set ntp <server> [<server>]    overrides DHCP option 42\n"
           "  set ntp dhcp                   back to DHCP option 42 / fallback\n");
    return 1;
}

static int cmd_save(int argc, char **argv)
{
    esp_err_t err = appcfg_save();
    printf("%s\n", err == ESP_OK ? "saved" : esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_reboot(int argc, char **argv)
{
    if (appcfg_dirty()) {
        printf("note: unsaved changes are lost\n");
    }
    sensors_save_state();
    printf("rebooting\n");
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    return 0;
}

static int cmd_factory_reset(int argc, char **argv)
{
    if (argc != 2 || strcmp(argv[1], "yes") != 0) {
        printf("erases all settings and the BSEC calibration state. Type: factory-reset yes\n");
        return 1;
    }
    esp_err_t err = appcfg_factory_reset();
    printf("NVS erased: %s, rebooting\n", esp_err_to_name(err));
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    return 0;
}

static int cmd_i2c(int argc, char **argv)
{
    if (argc != 2 || strcmp(argv[1], "scan") != 0) {
        printf("usage: i2c scan\n");
        return 1;
    }
    uint8_t found[128];
    int n = i2cbus_scan(found);
    printf("%d device(s):", n);
    for (int a = 0; a < 128; a++) {
        if (found[a]) {
            printf(" 0x%02x", a);
        }
    }
    printf("\n");
    return 0;
}

static void ow_print(void)
{
    sensors_ow_dev_t d[OW_MAX_DEVICES];
    int found = 0;
    int n = sensors_onewire_list(d, OW_MAX_DEVICES, &found);
    if (n < 0) {
        printf("no DS2484 found\n");
        return;
    }
    if (found > OW_MAX_DEVICES) {
        printf("ERROR: %d%s 1-Wire devices found, max %d - using the first %d\n", found,
               found >= OW_SEARCH_LIMIT ? "+" : "", OW_MAX_DEVICES, OW_MAX_DEVICES);
    }
    printf("%d device(s) in use\n", n);
    for (int i = 0; i < n; i++) {
        char rom[OW_ROM_STR_LEN];
        ow_rom_str(d[i].rom, rom);
        printf("  %d %s %-19s ", i + 1, rom, d[i].name);
        if (ow_family(d[i].rom) != OW_FAMILY_DS18B20) {
            printf("family 0x%02x, not read\n", ow_family(d[i].rom));
        } else if (d[i].valid) {
            printf("%7.2f C  crc errors %" PRIu32 "\n", d[i].temp_c, d[i].crc_errors);
        } else {
            printf("     --  crc errors %" PRIu32 "\n", d[i].crc_errors);
        }
    }
}

static int cmd_onewire(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "scan") == 0) {
        uint32_t before = sensors_onewire_rescan();
        for (int i = 0; i < 50 && sensors_onewire_scan_count() == before; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        ow_print();
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "list") == 0) {
        ow_print();
        return 0;
    }
    if (argc >= 3 && (strcmp(argv[1], "name") == 0 || strcmp(argv[1], "forget") == 0)) {
        uint64_t rom;
        if (!ow_rom_parse(argv[2], &rom)) {
            printf("ROM ID: 16 hex digits as shown by 'onewire scan'\n");
            return 1;
        }
        bool forget = strcmp(argv[1], "forget") == 0;
        if (!forget && argc != 4) {
            printf("usage: onewire name <rom> <name>\n");
            return 1;
        }
        esp_err_t err = ow_name_set(rom, forget ? NULL : argv[3]);
        if (err == ESP_ERR_INVALID_ARG) {
            printf("name: 1-%d chars [a-z0-9_], not starting with ow_\n", OW_NAME_LEN - 1);
        } else if (err == ESP_ERR_INVALID_STATE) {
            printf("name already used by another device\n");
        } else if (err == ESP_ERR_NO_MEM) {
            printf("name table full (%d) - 'onewire forget' an old one\n", OW_NAMES_MAX);
        } else if (err != ESP_OK) {
            printf("%s\n", esp_err_to_name(err));
        } else {
            printf("saved\n");
            sensors_onewire_rescan();   // picks up the new name
        }
        return err == ESP_OK ? 0 : 1;
    }
    if (argc >= 2 && strcmp(argv[1], "names") == 0) {
        uint64_t roms[OW_NAMES_MAX];
        char names[OW_NAMES_MAX][OW_NAME_LEN];
        int n = ow_names_list(roms, names, OW_NAMES_MAX);
        printf("%d of %d names stored\n", n, OW_NAMES_MAX);
        for (int i = 0; i < n; i++) {
            char rom[OW_ROM_STR_LEN];
            ow_rom_str(roms[i], rom);
            printf("  %s %s\n", rom, names[i]);
        }
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "selftest") == 0) {
        int n = argc >= 3 ? atoi(argv[2]) : OW_MAX_DEVICES + 1;
        char *rep = malloc(1024);
        if (!rep) {
            return 1;
        }
        bool ok = ow_selftest(n, rep, 1024);
        printf("%s", rep);
        free(rep);
        return ok ? 0 : 1;
    }
    printf("usage:\n"
           "  onewire scan                 search the bus, list devices\n"
           "  onewire list                 last search result\n"
           "  onewire name <rom> <name>    name a probe (saved at once)\n"
           "  onewire forget <rom>         remove a name\n"
           "  onewire names                stored names, also of absent probes\n"
           "  onewire selftest [n]         search on a simulated bus with n devices\n");
    return 1;
}

static int cmd_led(int argc, char **argv)
{
    if (argc == 2 && (strcmp(argv[1], "on") == 0 || strcmp(argv[1], "off") == 0)) {
        board_led_set(strcmp(argv[1], "on") == 0);
        return 0;
    }
    printf("usage: led on|off   (red LED D1 on GPIO41, off after boot)\n");
    return 1;
}

static int cmd_version(int argc, char **argv)
{
    const esp_app_desc_t *app = esp_app_get_description();
    printf("%s %s, built %s %s, IDF %s\n", app->project_name, app->version, app->date, app->time, app->idf_ver);
    return 0;
}

static int cmd_log(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "off") == 0) {
        esp_log_level_set("*", ESP_LOG_WARN);
    } else if (argc == 2 && strcmp(argv[1], "on") == 0) {
        esp_log_level_set("*", ESP_LOG_INFO);
    } else {
        printf("usage: log on|off   (off = warnings and errors only)\n");
        return 1;
    }
    return 0;
}

esp_err_t console_cmds_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t rc = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    rc.prompt = "hub> ";
    rc.task_stack_size = 6144;
    esp_console_dev_uart_config_t uc = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uc, &rc, &repl));

    const esp_console_cmd_t cmds[] = {
        {.command = "show", .help = "show status | show config", .func = cmd_show},
        {.command = "set", .help = "set wifi|hostname|tz|ntp ... (no args: usage)", .func = cmd_set},
        {.command = "save", .help = "write settings to NVS", .func = cmd_save},
        {.command = "reboot", .help = "restart (saves BSEC state)", .func = cmd_reboot},
        {.command = "factory-reset", .help = "erase all settings: factory-reset yes", .func = cmd_factory_reset},
        {.command = "i2c", .help = "i2c scan", .func = cmd_i2c},
        {.command = "onewire", .help = "onewire scan|list|name|forget|names|selftest", .func = cmd_onewire},
        {.command = "led", .help = "led on|off", .func = cmd_led},
        {.command = "version", .help = "firmware version", .func = cmd_version},
        {.command = "log", .help = "log on|off", .func = cmd_log},
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }
    esp_console_register_help_command();
    return esp_console_start_repl(repl);
}
