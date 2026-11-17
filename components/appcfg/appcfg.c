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
        ESP_LOGW(TAG, "NVS unusable (%s), erasing", esp_err_to_name(err));
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

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        char ssid[sizeof(s_cfg.wifi_ssid)] = "";
        load_str(h, "hostname", s_cfg.hostname, sizeof(s_cfg.hostname));
        load_str(h, "tz", s_cfg.tz, sizeof(s_cfg.tz));
        load_str(h, "ntp0", s_cfg.ntp[0], sizeof(s_cfg.ntp[0]));
        load_str(h, "ntp1", s_cfg.ntp[1], sizeof(s_cfg.ntp[1]));
        load_str(h, "ssid", ssid, sizeof(ssid));
        if (ssid[0]) {
            strlcpy(s_cfg.wifi_ssid, ssid, sizeof(s_cfg.wifi_ssid));
            s_cfg.wifi_psk[0] = '\0';
            load_str(h, "psk", s_cfg.wifi_psk, sizeof(s_cfg.wifi_psk));
            s_wifi_from_build = false;
        }
        nvs_close(h);
    }
    ESP_LOGI(TAG, "hostname=%s ssid='%s' (%s) tz=%s ntp=%s%s%s", s_cfg.hostname, s_cfg.wifi_ssid,
             s_wifi_from_build ? "wlan.config" : "nvs", s_cfg.tz,
             s_cfg.ntp[0][0] ? s_cfg.ntp[0] : "dhcp/fallback", s_cfg.ntp[1][0] ? " " : "", s_cfg.ntp[1]);
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
