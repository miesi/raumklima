#include "appcfg.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "wlan_defaults.h"

static const char *TAG = "appcfg";
static const char *NS = "appcfg";

static appcfg_t s_cfg;
static bool s_dirty;
static bool s_wifi_from_build;
static SemaphoreHandle_t s_lock;

static void load_str(nvs_handle_t h, const char *key, char *dst, size_t len)
{
    size_t n = len;
    char tmp[128];
    if (n > sizeof(tmp)) {
        n = sizeof(tmp);
    }
    if (nvs_get_str(h, key, tmp, &n) == ESP_OK) {
        strlcpy(dst, tmp, len);
    }
}

esp_err_t appcfg_init(void)
{
    s_lock = xSemaphoreCreateMutex();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "event=nvs_erase reason=%s", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // defaults
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_cfg.hostname, sizeof(s_cfg.hostname), "hub-%02x%02x%02x", mac[3], mac[4], mac[5]);
    // Central European Time with EU summer time: last Sunday in March 02:00 -> 03:00 local,
    // last Sunday in October 03:00 -> 02:00 local (POSIX TZ, newlib applies it every year).
    strlcpy(s_cfg.tz, APPCFG_TZ_DEFAULT, sizeof(s_cfg.tz));
    strlcpy(s_cfg.wifi_ssid, WLAN_DEFAULT_SSID, sizeof(s_cfg.wifi_ssid));
    strlcpy(s_cfg.wifi_psk, WLAN_DEFAULT_PSK, sizeof(s_cfg.wifi_psk));
    s_wifi_from_build = s_cfg.wifi_ssid[0] != '\0';
    s_cfg.display_s = APPCFG_INTERVAL_DEFAULT;
    s_cfg.carbon_s = APPCFG_INTERVAL_DEFAULT;
    s_cfg.carbon_port = APPCFG_CARBON_PORT;
    strlcpy(s_cfg.carbon_prefix, APPCFG_CARBON_PREFIX, sizeof(s_cfg.carbon_prefix));
    s_cfg.syslog_port = APPCFG_SYSLOG_PORT;
    s_cfg.syslog_level = APPCFG_SEV_INFO;

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        char ssid[sizeof(s_cfg.wifi_ssid)] = "";
        load_str(h, "hostname", s_cfg.hostname, sizeof(s_cfg.hostname));
        load_str(h, "tz", s_cfg.tz, sizeof(s_cfg.tz));
        load_str(h, "ntp0", s_cfg.ntp[0], sizeof(s_cfg.ntp[0]));
        load_str(h, "ntp1", s_cfg.ntp[1], sizeof(s_cfg.ntp[1]));
        nvs_get_u16(h, "disp_s", &s_cfg.display_s);
        nvs_get_u16(h, "carb_s", &s_cfg.carbon_s);
        load_str(h, "c_host", s_cfg.carbon_host, sizeof(s_cfg.carbon_host));
        nvs_get_u16(h, "c_port", &s_cfg.carbon_port);
        load_str(h, "c_prefix", s_cfg.carbon_prefix, sizeof(s_cfg.carbon_prefix));
        uint8_t u8;
        if (nvs_get_u8(h, "c_v4", &u8) == ESP_OK) {
            s_cfg.prefer_v4 = u8;
        }
        load_str(h, "s_host", s_cfg.syslog_host, sizeof(s_cfg.syslog_host));
        nvs_get_u16(h, "s_port", &s_cfg.syslog_port);
        if (nvs_get_u8(h, "s_tcp", &u8) == ESP_OK) {
            s_cfg.syslog_tcp = u8;
        }
        nvs_get_u8(h, "s_level", &s_cfg.syslog_level);
        load_str(h, "ssid", ssid, sizeof(ssid));
        if (ssid[0]) {
            strlcpy(s_cfg.wifi_ssid, ssid, sizeof(s_cfg.wifi_ssid));
            s_cfg.wifi_psk[0] = '\0';
            load_str(h, "psk", s_cfg.wifi_psk, sizeof(s_cfg.wifi_psk));
            s_wifi_from_build = false;
        }
        nvs_close(h);
    }
    if (s_cfg.display_s < APPCFG_INTERVAL_MIN || s_cfg.display_s > APPCFG_INTERVAL_MAX) {
        s_cfg.display_s = APPCFG_INTERVAL_DEFAULT;
    }
    if (s_cfg.carbon_s < APPCFG_INTERVAL_MIN || s_cfg.carbon_s > APPCFG_INTERVAL_MAX) {
        s_cfg.carbon_s = APPCFG_INTERVAL_DEFAULT;
    }
    ESP_LOGI(TAG, "event=loaded hostname=%s ssid=\"%s\" wifi_source=%s tz=%s ntp=\"%s%s%s\" display_s=%u carbon_s=%u",
             s_cfg.hostname, s_cfg.wifi_ssid, s_wifi_from_build ? "wlan.config" : "nvs", s_cfg.tz,
             s_cfg.ntp[0][0] ? s_cfg.ntp[0] : "dhcp", s_cfg.ntp[1][0] ? " " : "", s_cfg.ntp[1], s_cfg.display_s,
             s_cfg.carbon_s);
    return ESP_OK;
}

void appcfg_get(appcfg_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_cfg;
    xSemaphoreGive(s_lock);
}

static bool valid_hostname(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n > 32 || s[0] == '-' || s[n - 1] == '-') {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (!(islower((unsigned char)s[i]) || isdigit((unsigned char)s[i]) || s[i] == '-')) {
            return false;
        }
    }
    return true;
}

esp_err_t appcfg_set_hostname(const char *name)
{
    if (!valid_hostname(name)) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_cfg.hostname, name, sizeof(s_cfg.hostname));
    s_dirty = true;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t appcfg_set_wifi(const char *ssid, const char *psk)
{
    size_t ls = strlen(ssid), lp = strlen(psk);
    if (ls == 0 || ls > 32 || lp < 8 || lp > 64) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_cfg.wifi_ssid, ssid, sizeof(s_cfg.wifi_ssid));
    strlcpy(s_cfg.wifi_psk, psk, sizeof(s_cfg.wifi_psk));
    s_wifi_from_build = false;
    s_dirty = true;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t appcfg_set_tz(const char *tz)
{
    if (strlen(tz) == 0 || strlen(tz) >= sizeof(s_cfg.tz)) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_cfg.tz, tz, sizeof(s_cfg.tz));
    s_dirty = true;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t appcfg_set_ntp(const char *s1, const char *s2)
{
    if ((s1 && strlen(s1) >= sizeof(s_cfg.ntp[0])) || (s2 && strlen(s2) >= sizeof(s_cfg.ntp[1]))) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_cfg.ntp[0], s1 ? s1 : "", sizeof(s_cfg.ntp[0]));
    strlcpy(s_cfg.ntp[1], s2 ? s2 : "", sizeof(s_cfg.ntp[1]));
    s_dirty = true;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

static bool interval_ok(int s)
{
    return s >= APPCFG_INTERVAL_MIN && s <= APPCFG_INTERVAL_MAX;
}

esp_err_t appcfg_set_intervals(int display_s, int carbon_s)
{
    if ((display_s && !interval_ok(display_s)) || (carbon_s && !interval_ok(carbon_s))) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (display_s) {
        s_cfg.display_s = display_s;
    }
    if (carbon_s) {
        s_cfg.carbon_s = carbon_s;
    }
    s_dirty = true;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

// host name or address literal: letters, digits, '-', '.', ':' (IPv6), max 63
static bool valid_host(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n > 63) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (!(isalnum((unsigned char)s[i]) || s[i] == '-' || s[i] == '.' || s[i] == ':')) {
            return false;
        }
    }
    return true;
}

// Graphite path nodes: letters, digits, '_', '-', separated by single dots
static bool valid_prefix(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n > 47 || s[0] == '.' || s[n - 1] == '.') {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '.' && s[i + 1] == '.') {
            return false;
        }
        if (!(isalnum((unsigned char)s[i]) || s[i] == '_' || s[i] == '-' || s[i] == '.')) {
            return false;
        }
    }
    return true;
}

esp_err_t appcfg_set_carbon(const char *host, int port, const char *prefix)
{
    bool off = !host || !host[0];
    if ((!off && !valid_host(host)) || port < 0 || port > 65535 || (prefix && !valid_prefix(prefix))) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_cfg.carbon_host, off ? "" : host, sizeof(s_cfg.carbon_host));
    if (!off) {
        s_cfg.carbon_port = port ? port : APPCFG_CARBON_PORT;
    }
    if (prefix) {
        strlcpy(s_cfg.carbon_prefix, prefix, sizeof(s_cfg.carbon_prefix));
    }
    s_dirty = true;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

void appcfg_set_prefer_v4(bool v4)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg.prefer_v4 = v4;
    s_dirty = true;
    xSemaphoreGive(s_lock);
}

esp_err_t appcfg_set_syslog(const char *host, int port, bool tcp)
{
    bool off = !host || !host[0];
    if ((!off && !valid_host(host)) || port < 0 || port > 65535) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_cfg.syslog_host, off ? "" : host, sizeof(s_cfg.syslog_host));
    if (!off) {
        s_cfg.syslog_port = port ? port : APPCFG_SYSLOG_PORT;
        s_cfg.syslog_tcp = tcp;
    }
    s_dirty = true;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t appcfg_set_syslog_level(int severity)
{
    if (severity != APPCFG_SEV_ERROR && severity != APPCFG_SEV_WARNING && severity != APPCFG_SEV_INFO &&
        severity != APPCFG_SEV_DEBUG) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg.syslog_level = severity;
    s_dirty = true;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

bool appcfg_dirty(void)
{
    return s_dirty;
}

bool appcfg_wifi_from_build(void)
{
    return s_wifi_from_build;
}

esp_err_t appcfg_save(void)
{
    appcfg_t c;
    appcfg_get(&c);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    nvs_set_str(h, "hostname", c.hostname);
    nvs_set_str(h, "tz", c.tz);
    nvs_set_str(h, "ntp0", c.ntp[0]);
    nvs_set_str(h, "ntp1", c.ntp[1]);
    nvs_set_u16(h, "disp_s", c.display_s);
    nvs_set_u16(h, "carb_s", c.carbon_s);
    nvs_set_str(h, "c_host", c.carbon_host);
    nvs_set_u16(h, "c_port", c.carbon_port);
    nvs_set_str(h, "c_prefix", c.carbon_prefix);
    nvs_set_u8(h, "c_v4", c.prefer_v4);
    nvs_set_str(h, "s_host", c.syslog_host);
    nvs_set_u16(h, "s_port", c.syslog_port);
    nvs_set_u8(h, "s_tcp", c.syslog_tcp);
    nvs_set_u8(h, "s_level", c.syslog_level);
    if (!s_wifi_from_build) {
        nvs_set_str(h, "ssid", c.wifi_ssid);
        nvs_set_str(h, "psk", c.wifi_psk);
    }
    err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        s_dirty = false;
    }
    return err;
}

esp_err_t appcfg_factory_reset(void)
{
    nvs_flash_deinit();
    return nvs_flash_erase();
}
