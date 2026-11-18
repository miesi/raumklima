// Maxim DS2484 I2C-to-1-Wire bridge with DS18B20 temperature probes.
//
// Command set: same as DS2482-100 (sources: Linux drivers/w1/masters/ds2482.c, which also
// binds "maxim,ds2484"; the Analog Devices datasheets were not reachable when this was
// written - verify against the DS2484 datasheet before relying on details).
// Not yet tested on hardware.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "onewire.h"
#include "sensor_driver.h"
#include "sensors.h"

static const char *TAG = "ds2484";

#define CMD_DEVICE_RESET   0xF0
#define CMD_SET_READ_PTR   0xE1
#define CMD_WRITE_CONFIG   0xD2
#define CMD_1W_RESET       0xB4
#define CMD_1W_WRITE_BYTE  0xA5
#define CMD_1W_READ_BYTE   0x96
#define CMD_1W_TRIPLET     0x78
#define PTR_STATUS         0xF0
#define PTR_DATA           0xE1
#define PTR_CONFIG         0xC3

#define STS_1WB  0x01
#define STS_PPD  0x02
#define STS_SD   0x04
#define STS_LL   0x08
#define STS_RST  0x10
#define STS_SBR  0x20
#define STS_TSB  0x40
#define STS_DIR  0x80

#define CFG_APU  0x01      // active pull-up: better rise times on longer cables

#define DS18B20_CONVERT_T    0x44
#define DS18B20_READ_SCRATCH 0xBE
#define CONVERT_MS           800                        // 12 bit: 750 ms max (ASSUMPTION, DS18B20 datasheet)
#define RESCAN_US            (10LL * 60 * 1000 * 1000)  // pick up added/removed probes

static const uint8_t k_addrs[] = {0x18};

typedef struct {
    uint64_t rom;
    char name[OW_INSTANCE_LEN];
    float temp_c;
    bool valid;
    uint32_t crc_errors;
} probe_t;

typedef struct {
    sensor_inst_t *s;
    ow_bus_t bus;
    probe_t probe[OW_MAX_DEVICES];
    int n;                 // stored, <= OW_MAX_DEVICES
    int found;             // found on the bus, may be more
    uint32_t search_crc_errors;
    int64_t last_scan_us;
    uint32_t scan_count;
    bool rescan;
    bool converting;
    SemaphoreHandle_t lock;
} ctx_t;

static ctx_t *s_ctx;       // one DS2484 (fixed I2C address)

// --- DS2484 primitives ---

static esp_err_t cmd2(ctx_t *c, uint8_t cmd, uint8_t param)
{
    uint8_t b[2] = {cmd, param};
    return i2c_master_transmit(c->s->dev, b, 2, 50);
}

static esp_err_t cmd1(ctx_t *c, uint8_t cmd)
{
    return i2c_master_transmit(c->s->dev, &cmd, 1, 50);
}

static esp_err_t read1(ctx_t *c, uint8_t *b)
{
    return i2c_master_receive(c->s->dev, b, 1, 50);
}

// after a 1-Wire command the read pointer is at the status register
static esp_err_t wait_idle(ctx_t *c, uint8_t *status)
{
    int64_t t0 = esp_timer_get_time();
    do {
        esp_err_t err = read1(c, status);
        if (err != ESP_OK) {
            return err;
        }
        if (!(*status & STS_1WB)) {
            return ESP_OK;
        }
    } while (esp_timer_get_time() - t0 < 20000);
    return ESP_ERR_TIMEOUT;
}

static esp_err_t op_reset(void *ctx, bool *presence)
{
    ctx_t *c = ctx;
    uint8_t st;
    esp_err_t err = cmd1(c, CMD_1W_RESET);
    if (err == ESP_OK) {
        err = wait_idle(c, &st);
    }
    if (err == ESP_OK && (st & STS_SD)) {
        ESP_LOGW(TAG, "event=short_detected");
        return ESP_FAIL;
    }
    *presence = err == ESP_OK && (st & STS_PPD);
    return err;
}

static esp_err_t op_write(void *ctx, uint8_t b)
{
    ctx_t *c = ctx;
    uint8_t st;
    esp_err_t err = cmd2(c, CMD_1W_WRITE_BYTE, b);
    return err == ESP_OK ? wait_idle(c, &st) : err;
}

static esp_err_t op_read(void *ctx, uint8_t *b)
{
    ctx_t *c = ctx;
    uint8_t st;
    esp_err_t err = cmd1(c, CMD_1W_READ_BYTE);
    if (err == ESP_OK) {
        err = wait_idle(c, &st);
    }
    if (err == ESP_OK) {
        err = cmd2(c, CMD_SET_READ_PTR, PTR_DATA);
    }
    return err == ESP_OK ? read1(c, b) : err;
}

static esp_err_t op_triplet(void *ctx, bool dir, uint8_t *id, uint8_t *cmp, uint8_t *taken)
{
    ctx_t *c = ctx;
    uint8_t st;
    esp_err_t err = cmd2(c, CMD_1W_TRIPLET, dir ? 0xFF : 0x00);   // direction in bit 7
    if (err == ESP_OK) {
        err = wait_idle(c, &st);
    }
    if (err == ESP_OK) {
        *id = !!(st & STS_SBR);
        *cmp = !!(st & STS_TSB);
        *taken = !!(st & STS_DIR);
    }
    return err;
}

// --- driver ---

static void scan(ctx_t *c)
{
    uint64_t roms[OW_MAX_DEVICES];
    int found = ow_search(&c->bus, roms, OW_MAX_DEVICES, &c->search_crc_errors);
    xSemaphoreTake(c->lock, portMAX_DELAY);
    c->found = found < 0 ? 0 : found;
    c->n = c->found < OW_MAX_DEVICES ? c->found : OW_MAX_DEVICES;
    for (int i = 0; i < c->n; i++) {
        probe_t *p = &c->probe[i];
        if (p->rom != roms[i]) {
            memset(p, 0, sizeof(*p));
            p->rom = roms[i];
        }
        ow_name_get(p->rom, p->name, sizeof(p->name));
    }
    if (c->found > OW_MAX_DEVICES) {
        snprintf(c->s->warn, sizeof(c->s->warn), "%d%s 1-Wire devices, max %d", c->found,
                 c->found >= OW_SEARCH_LIMIT ? "+" : "", OW_MAX_DEVICES);
        ESP_LOGE(TAG, "event=too_many_devices found=%d%s max=%d using_first=%d", c->found,
                 c->found >= OW_SEARCH_LIMIT ? "+" : "",
                 OW_MAX_DEVICES, OW_MAX_DEVICES);
    } else {
        c->s->warn[0] = '\0';
    }
    snprintf(c->s->info, sizeof(c->s->info), "%d 1-Wire device(s)", c->found);
    xSemaphoreGive(c->lock);
    if (found < 0) {
        ESP_LOGW(TAG, "event=search_failed");
    } else {
        ESP_LOGI(TAG, "event=search devices=%d", found);
    }
    c->last_scan_us = esp_timer_get_time();
    c->rescan = false;
    c->converting = false;   // the search disturbed any running conversion
    c->scan_count++;
}

static esp_err_t probe(sensor_inst_t *s)
{
    if (s_ctx) {
        strlcpy(s->model, "DS2484", sizeof(s->model));
        strlcpy(s->name, "ds2484", sizeof(s->name));
        strlcpy(s->info, "second DS2484 - not supported", sizeof(s->info));
        s->supported = false;
        return ESP_OK;
    }
    // Device Reset leaves the read pointer on the status register; expected RST set,
    // LL = line level (1 with a pulled-up bus), everything else 0.
    uint8_t c = CMD_DEVICE_RESET, st = 0;
    if (i2c_master_transmit(s->dev, &c, 1, 50) != ESP_OK || i2c_master_receive(s->dev, &st, 1, 50) != ESP_OK ||
        (st & ~STS_LL) != STS_RST) {
        return ESP_ERR_NOT_FOUND;
    }
    strlcpy(s->model, "DS2484", sizeof(s->model));
    strlcpy(s->name, "ds2484", sizeof(s->name));

    ctx_t *x = calloc(1, sizeof(ctx_t));
    if (!x) {
        return ESP_ERR_NO_MEM;
    }
    x->s = s;
    x->lock = xSemaphoreCreateMutex();
    x->bus = (ow_bus_t){op_reset, op_write, op_read, op_triplet, x};
    x->rescan = true;
    s->ctx = x;
    s_ctx = x;

    // config: upper nibble = one's complement of the lower nibble
    uint8_t cfg = CFG_APU;
    if (cmd2(x, CMD_WRITE_CONFIG, (uint8_t)((~cfg << 4) | cfg)) != ESP_OK) {
        return ESP_FAIL;
    }
    s->supported = true;
    return ESP_OK;
}

static int64_t poll(sensor_inst_t *s, int64_t now_us)
{
    ctx_t *c = s->ctx;
    if (c->last_scan_us == 0) {
        scan(c);   // first cycle: find the probes before reading them; later searches: after_cycle()
    }
    if (c->n == 0) {
        return SENSOR_DONE;
    }
    if (!c->converting) {
        // all probes convert at once
        bool presence = false;
        esp_err_t err = op_reset(c, &presence);
        if (err == ESP_OK && !presence) {
            err = ESP_ERR_NOT_FOUND;   // no presence pulse: nothing on the 1-Wire bus
        }
        if (err == ESP_OK) {
            err = op_write(c, OW_CMD_SKIP_ROM);
        }
        if (err == ESP_OK) {
            err = op_write(c, DS18B20_CONVERT_T);
        }
        if (err != ESP_OK) {
            return sensor_fail(s, err);
        }
        c->converting = true;
        return now_us + CONVERT_MS * 1000LL;
    }
    c->converting = false;
    int ok = 0;
    for (int i = 0; i < c->n; i++) {
        probe_t *p = &c->probe[i];
        if (ow_family(p->rom) != OW_FAMILY_DS18B20) {
            continue;
        }
        uint8_t sp[9];
        esp_err_t err = ow_select(&c->bus, p->rom);
        if (err == ESP_OK) {
            err = op_write(c, DS18B20_READ_SCRATCH);
        }
        for (int j = 0; j < 9 && err == ESP_OK; j++) {
            err = op_read(c, &sp[j]);
        }
        xSemaphoreTake(c->lock, portMAX_DELAY);
        if (err != ESP_OK || ow_crc8(sp, 8) != sp[8]) {
            p->crc_errors++;
            p->valid = false;
            ESP_LOGW(s->name, "event=probe_failed probe=%s rom=%016llx err=%s crc_errors=%lu", p->name,
                     (unsigned long long)p->rom, err != ESP_OK ? esp_err_to_name(err) : "CRC", (unsigned long)p->crc_errors);
        } else {
            p->temp_c = (int16_t)((sp[1] << 8) | sp[0]) / 16.0f;   // 12 bit, 0.0625 C per LSB
            p->valid = true;
            ok++;
        }
        xSemaphoreGive(c->lock);
        if (p->valid) {
            sensor_publish_as(p->name, "temperature_c", p->temp_c);
        }
    }
    return ok > 0 ? SENSOR_DONE : sensor_fail(s, ESP_ERR_INVALID_CRC);
}

// after a read cycle, outside its timed part: a requested search or the periodic one
static void after_cycle(sensor_inst_t *s)
{
    ctx_t *c = s->ctx;
    if (c->rescan || esp_timer_get_time() - c->last_scan_us > RESCAN_US) {
        scan(c);
    }
}

static void detail(sensor_inst_t *s, char *buf, size_t len)
{
    ctx_t *c = s->ctx;
    xSemaphoreTake(c->lock, portMAX_DELAY);
    size_t o = 0;
    if (c->found > OW_MAX_DEVICES) {
        o += snprintf(buf + o, len - o, "ERROR: %d%s devices, max %d - first %d shown\n", c->found,
                      c->found >= OW_SEARCH_LIMIT ? "+" : "", OW_MAX_DEVICES,
                      OW_MAX_DEVICES);
    }
    for (int i = 0; i < c->n && o < len; i++) {
        const probe_t *p = &c->probe[i];
        char rom[OW_ROM_STR_LEN];
        ow_rom_str(p->rom, rom);
        if (ow_family(p->rom) != OW_FAMILY_DS18B20) {
            o += snprintf(buf + o, len - o, "%-19s family %02x\n", p->name, ow_family(p->rom));
        } else if (p->valid) {
            o += snprintf(buf + o, len - o, "%-19s %6.2f \xc2\xb0" "C\n", p->name, p->temp_c);
        } else {
            o += snprintf(buf + o, len - o, "%-19s     --\n", p->name);
        }
    }
    if (c->n == 0 && o < len) {
        snprintf(buf + o, len - o, "no 1-Wire devices found");
    }
    xSemaphoreGive(c->lock);
}

const sensor_driver_t sensor_drv_ds2484 = {
    .driver = "ds2484",
    .addrs = k_addrs,
    .n_addrs = sizeof(k_addrs),
    .probe = probe,
    .poll = poll,
    .after_cycle = after_cycle,
    .detail = detail,
};

// --- API for the console (sensors.h) ---

int sensors_onewire_list(sensors_ow_dev_t *out, int max, int *found)
{
    ctx_t *c = s_ctx;
    if (!c) {
        return -1;
    }
    xSemaphoreTake(c->lock, portMAX_DELAY);
    int n = c->n < max ? c->n : max;
    for (int i = 0; i < n; i++) {
        out[i].rom = c->probe[i].rom;
        strlcpy(out[i].name, c->probe[i].name, sizeof(out[i].name));
        out[i].temp_c = c->probe[i].temp_c;
        out[i].valid = c->probe[i].valid;
        out[i].crc_errors = c->probe[i].crc_errors;
    }
    if (found) {
        *found = c->found;
    }
    xSemaphoreGive(c->lock);
    return n;
}

uint32_t sensors_onewire_rescan(void)
{
    if (!s_ctx) {
        return 0;
    }
    uint32_t before = s_ctx->scan_count;
    s_ctx->rescan = true;
    sensors_kick(s_ctx->s);   // the sensors task does the search
    return before;
}

uint32_t sensors_onewire_scan_count(void)
{
    return s_ctx ? s_ctx->scan_count : 0;
}
