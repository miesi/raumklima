#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "onewire.h"

#define NVS_NS  "onewire"
#define NVS_KEY "names"

typedef struct {
    uint64_t rom;
    char name[OW_NAME_LEN];
} entry_t;

static entry_t s_tab[OW_NAMES_MAX];
static int s_n;
static SemaphoreHandle_t s_lock;

esp_err_t ow_names_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return ESP_OK;
    }
    size_t len = sizeof(s_tab);
    if (nvs_get_blob(h, NVS_KEY, s_tab, &len) == ESP_OK) {
        s_n = len / sizeof(entry_t);
    }
    nvs_close(h);
    return ESP_OK;
}

static esp_err_t persist(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = s_n ? nvs_set_blob(h, NVS_KEY, s_tab, s_n * sizeof(entry_t)) : nvs_erase_key(h, NVS_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

bool ow_name_get(uint64_t rom, char *out, size_t len)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_n; i++) {
        if (s_tab[i].rom == rom) {
            strlcpy(out, s_tab[i].name, len);
            xSemaphoreGive(s_lock);
            return true;
        }
    }
    xSemaphoreGive(s_lock);
    char r[OW_ROM_STR_LEN];
    ow_rom_str(rom, r);
    snprintf(out, len, "ow_%s", r);
    return false;
}

static bool valid_name(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n >= OW_NAME_LEN || strncmp(s, "ow_", 3) == 0) {
        return false;   // "ow_" is reserved for unnamed devices
    }
    for (size_t i = 0; i < n; i++) {
        if (!(islower((unsigned char)s[i]) || isdigit((unsigned char)s[i]) || s[i] == '_')) {
            return false;
        }
    }
    return true;
}

esp_err_t ow_name_set(uint64_t rom, const char *name)
{
    bool remove = !name || !name[0];
    if (!remove && !valid_name(name)) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int idx = -1;
    for (int i = 0; i < s_n; i++) {
        if (s_tab[i].rom == rom) {
            idx = i;
        } else if (!remove && strcmp(s_tab[i].name, name) == 0) {
            xSemaphoreGive(s_lock);
            return ESP_ERR_INVALID_STATE;   // name already used by another device
        }
    }
    esp_err_t err = ESP_OK;
    if (remove) {
        if (idx >= 0) {
            s_tab[idx] = s_tab[--s_n];
        }
    } else if (idx >= 0) {
        strlcpy(s_tab[idx].name, name, OW_NAME_LEN);
    } else if (s_n < OW_NAMES_MAX) {
        s_tab[s_n].rom = rom;
        strlcpy(s_tab[s_n].name, name, OW_NAME_LEN);
        s_n++;
    } else {
        err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) {
        err = persist();
    }
    xSemaphoreGive(s_lock);
    return err;
}

int ow_names_list(uint64_t *roms, char (*names)[OW_NAME_LEN], int max)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_n < max ? s_n : max;
    for (int i = 0; i < n; i++) {
        roms[i] = s_tab[i].rom;
        strlcpy(names[i], s_tab[i].name, OW_NAME_LEN);
    }
    xSemaphoreGive(s_lock);
    return n;
}
