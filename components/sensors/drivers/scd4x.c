// Sensirion SCD4x (SCD40/41/42/43) CO2 sensor in periodic measurement mode (5 s).
// Commands, delays, conversions and the FRC / temperature offset procedures follow Sensirion's
// embedded-i2c-scd4x (BSD-3-Clause) and its documentation comments.
//
// - Pressure compensation: the primary pressure metric (BME6xx) once a minute.
// - Temperature offset: "scd41 offset auto" computes it against the primary reference
//   temperature (SHT4x): Toffset_new = T_SCD - T_ref + Toffset_old. Stored in NVS and written
//   to the sensor at every boot (no persist_settings, spares the sensor EEPROM).
// - Forced recalibration (FRC): console "scd41 frc <ppm>" or a long press on BOOT ("I am in
//   fresh air", target = "set co2 outdoor", default 430 ppm). Requires 3 min of periodic
//   operation in constant air; the firmware checks that CO2 was stable and refuses otherwise.
// - ASC target = outdoor value, set at boot (factory default 400 ppm).
// - Fresh-air hint: no CO2 reading below 450 ppm for more than 156 h (the ASC standard period)
//   gives a warning on the display.
//
// All sensor commands run in the sensors task; console and keys only post requests.
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "metrics.h"
#include "nvs.h"
#include "sensor_driver.h"
#include "sensors.h"

static const char *TAG = "scd4x";

#define CMD_START_PERIODIC   0x21B1
#define CMD_STOP_PERIODIC    0x3F86   // 500 ms
#define CMD_READ_MEASUREMENT 0xEC05   // 1 ms, 3 words
#define CMD_DATA_READY       0xE4B8   // 1 ms, ready if bits 10..0 != 0
#define CMD_SET_PRESSURE     0xE000   // 1 ms, word = hPa
#define CMD_SET_TEMP_OFFSET  0x241D   // idle only, raw = T * 65535 / 175
#define CMD_GET_TEMP_OFFSET  0x2318
#define CMD_FRC              0x362F   // idle only, 400 ms, returns correction + 0x8000
#define CMD_SET_ASC_TARGET   0x243A   // idle only, ppm
#define CMD_REINIT           0x3646   // idle only, 30 ms, reloads user settings from EEPROM
#define CMD_GET_ASC_ENABLED  0x2313
#define CMD_GET_ASC_TARGET   0x233F
#define CMD_SELF_TEST        0x3639   // idle only, 10 s, 0 = no malfunction
#define CMD_SERIAL           0x3682
#define CMD_VARIANT          0x202F

#define PERIOD_US            (5LL * 1000 * 1000)
#define PRESSURE_EVERY_US    (60LL * 1000 * 1000)
#define FRC_MIN_RUN_US       (3LL * 60 * 1000 * 1000)   // Sensirion: >= 3 min periodic operation
#define STABLE_WINDOW        36                          // samples = 3 min at 5 s
#define STABLE_MAX_SPAN_PPM  40                          // ASSUMPTION: our threshold for "constant"
#define SESSION_TIMEOUT_US   (30LL * 60 * 1000 * 1000)   // fresh-air session gives up after 30 min
#define FRESH_PPM            450
#define CO2_PLAUSIBLE_MIN    300   // below any real air (outdoor ~430): sensor problem, not data
#define FRESH_WARN_H         156
#define OFFSET_AVG_N         24                          // 2 min of differences for "offset auto"
#define NVS_NS               "scd4x"

static const uint8_t k_addrs[] = {0x62};

typedef enum { REQ_NONE, REQ_OFFSET_AUTO, REQ_OFFSET_SET, REQ_OFFSET_RESET, REQ_FRC, REQ_FRC_FORCE, REQ_DIAG } req_t;

typedef struct {
    sensor_inst_t *s;
    SemaphoreHandle_t lock;
    SemaphoreHandle_t done;
    int64_t started_us;             // periodic measurement running since
    int64_t last_pressure_us;
    uint16_t pressure_hpa;
    // CO2 ring for the stability check
    uint16_t co2[STABLE_WINDOW];
    int n_co2, i_co2;
    // T_scd - T_ref running mean
    float diff_sum;
    int diff_n;
    // persistent
    float offset_c;
    uint16_t outdoor_ppm;
    int64_t frc_utc;
    uint16_t frc_target;
    int16_t frc_correction;
    int64_t fresh_utc;              // last CO2 < FRESH_PPM, 0 = unknown
    // request from console/keys
    req_t req;
    float req_arg;
    esp_err_t req_err;
    char req_msg[200];
    // fresh-air session (BOOT long press)
    bool session;
    int64_t session_start_us;
    char session_msg[64];
    int64_t session_msg_until_us;
    bool asc_target_ok;
    uint32_t implausible;           // consecutive readings below CO2_PLAUSIBLE_MIN
} ctx_t;

static ctx_t s_c;

// --- I2C ---

static esp_err_t send(sensor_inst_t *s, uint16_t cmd)
{
    uint8_t b[2] = {cmd >> 8, cmd & 0xff};
    return i2c_master_transmit(s->dev, b, 2, 50);
}

static esp_err_t send_word(sensor_inst_t *s, uint16_t cmd, uint16_t w)
{
    uint8_t b[5] = {cmd >> 8, cmd & 0xff, w >> 8, w & 0xff, 0};
    b[4] = sensirion_crc8(&b[2], 2);
    return i2c_master_transmit(s->dev, b, 5, 50);
}

static esp_err_t receive_words(sensor_inst_t *s, uint16_t *w, int n)
{
    uint8_t b[9];
    esp_err_t err = i2c_master_receive(s->dev, b, n * 3, 50);
    if (err != ESP_OK) {
        return err;
    }
    for (int i = 0; i < n; i++) {
        if (sensirion_crc8(&b[3 * i], 2) != b[3 * i + 2]) {
            return ESP_ERR_INVALID_CRC;
        }
        w[i] = (b[3 * i] << 8) | b[3 * i + 1];
    }
    return ESP_OK;
}

static esp_err_t read_words(sensor_inst_t *s, uint16_t cmd, uint16_t *w, int n)
{
    esp_err_t err = send(s, cmd);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(2));
    return receive_words(s, w, n);
}

static esp_err_t stop_periodic(sensor_inst_t *s)
{
    esp_err_t err = send(s, CMD_STOP_PERIODIC);
    vTaskDelay(pdMS_TO_TICKS(500));
    return err;
}

static esp_err_t start_periodic(sensor_inst_t *s)
{
    esp_err_t err = send(s, CMD_START_PERIODIC);
    s_c.started_us = esp_timer_get_time();
    s_c.n_co2 = s_c.i_co2 = 0;     // the stability window starts again
    return err;
}

// --- NVS ---

static void nvs_load(void)
{
    s_c.outdoor_ppm = 430;   // NOAA Mauna Loa, June 2026: 431.44 ppm (spec); rounded down
    s_c.offset_c = NAN;      // NAN = never calibrated, keep the sensor's own value
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint32_t u32;
    int64_t i64;
    uint16_t u16;
    int16_t i16;
    if (nvs_get_u32(h, "offset", &u32) == ESP_OK) {
        memcpy(&s_c.offset_c, &u32, 4);
    }
    if (nvs_get_u16(h, "outdoor", &u16) == ESP_OK) {
        s_c.outdoor_ppm = u16;
    }
    if (nvs_get_i64(h, "frc_utc", &i64) == ESP_OK) {
        s_c.frc_utc = i64;
    }
    if (nvs_get_u16(h, "frc_tgt", &u16) == ESP_OK) {
        s_c.frc_target = u16;
    }
    if (nvs_get_i16(h, "frc_corr", &i16) == ESP_OK) {
        s_c.frc_correction = i16;
    }
    if (nvs_get_i64(h, "fresh_utc", &i64) == ESP_OK) {
        s_c.fresh_utc = i64;
    }
    nvs_close(h);
}

static void nvs_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (!isnan(s_c.offset_c)) {
        uint32_t u32;
        memcpy(&u32, &s_c.offset_c, 4);
        nvs_set_u32(h, "offset", u32);
    }
    nvs_set_u16(h, "outdoor", s_c.outdoor_ppm);
    nvs_set_i64(h, "frc_utc", s_c.frc_utc);
    nvs_set_u16(h, "frc_tgt", s_c.frc_target);
    nvs_set_i16(h, "frc_corr", s_c.frc_correction);
    nvs_set_i64(h, "fresh_utc", s_c.fresh_utc);
    nvs_commit(h);
    nvs_close(h);
}

static time_t utc_now(void)
{
    time_t t = time(NULL);
    return t > 1700000000 ? t : 0;
}

// --- idle-mode helpers (caller stopped periodic measurement) ---

static esp_err_t write_offset(sensor_inst_t *s, float c)
{
    uint16_t raw = (uint16_t)lroundf(c * 65535.0f / 175.0f);
    esp_err_t err = send_word(s, CMD_SET_TEMP_OFFSET, raw);
    vTaskDelay(pdMS_TO_TICKS(2));
    return err;
}

static esp_err_t read_offset(sensor_inst_t *s, float *c)
{
    uint16_t raw;
    esp_err_t err = read_words(s, CMD_GET_TEMP_OFFSET, &raw, 1);
    if (err == ESP_OK) {
        *c = raw * 175.0f / 65535.0f;
    }
    return err;
}

static esp_err_t do_frc(sensor_inst_t *s, uint16_t ppm, int16_t *correction)
{
    esp_err_t err = send_word(s, CMD_FRC, ppm);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(400));
    uint16_t w;
    err = receive_words(s, &w, 1);
    if (err != ESP_OK) {
        return err;
    }
    if (w == 0xFFFF) {
        return ESP_ERR_INVALID_STATE;   // sensor was not operated before the command
    }
    *correction = (int16_t)(w - 0x8000);
    return ESP_OK;
}

// --- stability ---

static bool co2_stable(int *span_out)
{
    if (s_c.n_co2 < STABLE_WINDOW) {
        *span_out = -1;
        return false;
    }
    uint16_t lo = 0xffff, hi = 0;
    for (int i = 0; i < STABLE_WINDOW; i++) {
        lo = s_c.co2[i] < lo ? s_c.co2[i] : lo;
        hi = s_c.co2[i] > hi ? s_c.co2[i] : hi;
    }
    *span_out = hi - lo;
    return hi - lo <= STABLE_MAX_SPAN_PPM;
}

// FRC with checks; runs in the sensors task
static esp_err_t frc_checked(sensor_inst_t *s, uint16_t target, bool force, char *msg, size_t len)
{
    int64_t now = esp_timer_get_time();
    int span;
    bool stable = co2_stable(&span);
    if (!force && now - s_c.started_us < FRC_MIN_RUN_US) {
        snprintf(msg, len, "rejected: sensor running %lld s, needs 180 s", (long long)((now - s_c.started_us) / 1000000));
        return ESP_ERR_INVALID_STATE;
    }
    if (!force && !stable) {
        if (span < 0) {
            snprintf(msg, len, "rejected: not enough readings for the 3 min stability window");
        } else {
            snprintf(msg, len, "rejected: CO2 varied by %d ppm in 3 min (max %d)", span, STABLE_MAX_SPAN_PPM);
        }
        return ESP_ERR_INVALID_STATE;
    }
    stop_periodic(s);
    int16_t corr = 0;
    esp_err_t err = do_frc(s, target, &corr);
    start_periodic(s);
    if (err != ESP_OK) {
        snprintf(msg, len, "FRC failed: %s", esp_err_to_name(err));
        return err;
    }
    s_c.frc_utc = utc_now();
    s_c.frc_target = target;
    s_c.frc_correction = corr;
    nvs_save();
    snprintf(msg, len, "FRC to %u ppm done, correction %+d ppm", target, corr);
    ESP_LOGI(TAG, "%s", msg);
    return ESP_OK;
}

static void handle_request(sensor_inst_t *s)
{
    req_t r = s_c.req;
    if (r == REQ_NONE) {
        return;
    }
    esp_err_t err = ESP_OK;
    char *msg = s_c.req_msg;
    size_t len = sizeof(s_c.req_msg);
    if (r == REQ_DIAG) {
        // read-only checks; the self test does not touch the calibration
        uint16_t asc_en = 0xffff, asc_tgt = 0xffff, st = 0xffff;
        float off = NAN;
        stop_periodic(s);
        esp_err_t e1 = read_words(s, CMD_GET_ASC_ENABLED, &asc_en, 1);
        esp_err_t e2 = read_words(s, CMD_GET_ASC_TARGET, &asc_tgt, 1);
        read_offset(s, &off);
        esp_err_t e3 = send(s, CMD_SELF_TEST);
        vTaskDelay(pdMS_TO_TICKS(10500));
        if (e3 == ESP_OK) {
            e3 = receive_words(s, &st, 1);
        }
        start_periodic(s);
        snprintf(msg, len, "ASC enabled %u (%s), ASC target %u ppm (%s), T offset %.2f C, self test %s: 0x%04x%s", asc_en,
                 esp_err_to_name(e1), asc_tgt, esp_err_to_name(e2), off, esp_err_to_name(e3), st,
                 st == 0 ? " = no malfunction" : " = MALFUNCTION");
        err = e3;
        goto out;
    }
    if (r == REQ_OFFSET_RESET) {
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_key(h, "offset");
            nvs_commit(h);
            nvs_close(h);
        }
        s_c.offset_c = NAN;
        stop_periodic(s);
        err = send(s, CMD_REINIT);
        vTaskDelay(pdMS_TO_TICKS(30));
        float cur = NAN;
        read_offset(s, &cur);
        send_word(s, CMD_SET_ASC_TARGET, s_c.outdoor_ppm);   // reinit reloaded the EEPROM value too
        vTaskDelay(pdMS_TO_TICKS(2));
        start_periodic(s);
        snprintf(msg, len, "offset removed from NVS, sensor EEPROM value %.2f C", cur);
        goto out;
    }
    if (r == REQ_OFFSET_AUTO || r == REQ_OFFSET_SET) {
        float new_c;
        if (r == REQ_OFFSET_AUTO) {
            if (s_c.diff_n < OFFSET_AVG_N) {
                snprintf(msg, len, "rejected: %d of %d paired readings with the reference yet", s_c.diff_n, OFFSET_AVG_N);
                err = ESP_ERR_INVALID_STATE;
                goto out;
            }
            float cur;
            stop_periodic(s);
            err = read_offset(s, &cur);
            if (err != ESP_OK) {
                start_periodic(s);
                snprintf(msg, len, "reading the offset failed: %s", esp_err_to_name(err));
                goto out;
            }
            float d = s_c.diff_sum / s_c.diff_n;
            new_c = d + cur;   // Toffset_actual = T_SCD - T_ref + Toffset_previous
            snprintf(msg, len, "offset %.2f -> %.2f C (T_SCD - T_ref = %+.2f C over %d readings)", cur, new_c, d,
                     s_c.diff_n);
        } else {
            new_c = s_c.req_arg;
            stop_periodic(s);
            snprintf(msg, len, "offset set to %.2f C", new_c);
        }
        if (new_c < 0 || new_c > 20) {
            start_periodic(s);
            snprintf(msg, len, "rejected: offset %.2f C outside 0..20 C (Sensirion recommendation)", new_c);
            err = ESP_ERR_INVALID_ARG;
            goto out;
        }
        err = write_offset(s, new_c);
        start_periodic(s);
        if (err == ESP_OK) {
            s_c.offset_c = new_c;
            s_c.diff_sum = 0;
            s_c.diff_n = 0;
            nvs_save();
        }
    } else {
        err = frc_checked(s, (uint16_t)s_c.req_arg, r == REQ_FRC_FORCE, msg, len);
    }
out:
    s_c.req_err = err;
    s_c.req = REQ_NONE;
    xSemaphoreGive(s_c.done);
}

// --- driver ---

static esp_err_t probe(sensor_inst_t *s)
{
    // may still measure from before a soft reboot; most commands need idle mode
    stop_periodic(s);
    uint16_t sn[3], var;
    if (read_words(s, CMD_SERIAL, sn, 3) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    const char *model = "SCD4x";
    if (read_words(s, CMD_VARIANT, &var, 1) == ESP_OK) {
        switch (var & 0xF000) {
        case 0x0000: model = "SCD40"; break;
        case 0x1000: model = "SCD41"; break;
        case 0x2000: model = "SCD42"; break;
        case 0x5000: model = "SCD43"; break;
        default: break;
        }
    }
    strlcpy(s->model, model, sizeof(s->model));
    snprintf(s->name, sizeof(s->name), "%c%c%c%c%c", model[0] | 0x20, model[1] | 0x20, model[2] | 0x20, model[3],
             model[4]);   // "scd41"
    snprintf(s->info, sizeof(s->info), "serial %04x%04x%04x", sn[0], sn[1], sn[2]);

    s_c.s = s;
    s_c.lock = xSemaphoreCreateMutex();
    s_c.done = xSemaphoreCreateBinary();
    s_c.last_pressure_us = -PRESSURE_EVERY_US;   // first compensation right away
    nvs_load();
    s->ctx = &s_c;

    if (!isnan(s_c.offset_c)) {
        esp_err_t err = write_offset(s, s_c.offset_c);
        ESP_LOGI(TAG, "temperature offset %.2f C from NVS: %s", s_c.offset_c, esp_err_to_name(err));
    } else {
        float cur = NAN;
        read_offset(s, &cur);
        s_c.offset_c = NAN;
        ESP_LOGI(TAG, "temperature offset %.2f C (sensor), not calibrated - 'scd41 offset auto'", cur);
    }
    esp_err_t err = send_word(s, CMD_SET_ASC_TARGET, s_c.outdoor_ppm);
    vTaskDelay(pdMS_TO_TICKS(2));
    s_c.asc_target_ok = err == ESP_OK;
    ESP_LOGI(TAG, "ASC target %u ppm: %s", s_c.outdoor_ppm, esp_err_to_name(err));

    if (start_periodic(s) != ESP_OK) {
        return ESP_FAIL;
    }
    s->supported = true;
    return ESP_OK;
}

static void compensate_pressure(sensor_inst_t *s, int64_t now_us)
{
    metric_t p;
    if (now_us - s_c.last_pressure_us < PRESSURE_EVERY_US || !metrics_get_primary("pressure_hpa", &p)) {
        return;
    }
    uint16_t hpa = (uint16_t)(p.value + 0.5f);
    if (send_word(s, CMD_SET_PRESSURE, hpa) == ESP_OK) {
        if (hpa != s_c.pressure_hpa) {
            ESP_LOGI(TAG, "ambient pressure compensation %u hPa", hpa);
        }
        s_c.pressure_hpa = hpa;
        s_c.last_pressure_us = now_us;
        sensor_publish(s, "pressure_comp_hpa", hpa);
    }
}

static void update_warn(sensor_inst_t *s, int64_t now_us)
{
    char w[sizeof(s->warn)] = "";
    time_t now = utc_now();
    if (s_c.implausible) {
        snprintf(w, sizeof(w), "SCD41: CO2 %s - sensor problem, 'scd41 diag'", "implausible (< 300 ppm)");
    } else if (s_c.session) {
        int span;
        co2_stable(&span);
        int el = (now_us - s_c.session_start_us) / 1000000;
        snprintf(w, sizeof(w), "fresh-air cal.: %d:%02d, span %d ppm", el / 60, el % 60, span < 0 ? 0 : span);
    } else if (s_c.session_msg[0] && now_us < s_c.session_msg_until_us) {
        strlcpy(w, s_c.session_msg, sizeof(w));
    } else if (now && s_c.fresh_utc && now - s_c.fresh_utc > FRESH_WARN_H * 3600) {
        snprintf(w, sizeof(w), "no fresh air for %lld h - please ventilate", (long long)((now - s_c.fresh_utc) / 3600));
    }
    if (strcmp(w, s->warn) != 0) {
        if (w[0]) {
            ESP_LOGW(TAG, "%s", w);
        }
        strlcpy(s->warn, w, sizeof(s->warn));
    }
}

static int64_t poll(sensor_inst_t *s, int64_t now_us)
{
    handle_request(s);

    uint16_t ready;
    if (read_words(s, CMD_DATA_READY, &ready, 1) != ESP_OK) {
        return -1;
    }
    if ((ready & 0x07FF) == 0) {
        return now_us + 500 * 1000;   // not yet, look again soon
    }
    uint16_t w[3];
    if (read_words(s, CMD_READ_MEASUREMENT, w, 3) != ESP_OK) {
        return -1;
    }
    float t = -45.0f + 175.0f * w[1] / 65536.0f;   // ((21875 * t) >> 13) - 45000
    sensor_publish(s, "temperature_c", t);
    sensor_publish(s, "humidity_pct", 100.0f * w[2] / 65536.0f);   // (12500 * h) >> 13
    if (w[0] < CO2_PLAUSIBLE_MIN) {
        // not published: no "0 ppm" on the display, no fake fresh air, no FRC on it
        if (s_c.implausible++ == 0) {
            ESP_LOGE(TAG, "CO2 reading %u ppm is implausible - sensor problem, run 'scd41 diag'", w[0]);
        }
        sensor_publish(s, "co2_raw_ppm", w[0]);
        s_c.n_co2 = s_c.i_co2 = 0;
        compensate_pressure(s, now_us);
        update_warn(s, now_us);
        return now_us + PERIOD_US;
    }
    if (s_c.implausible) {
        ESP_LOGI(TAG, "CO2 plausible again (%u ppm) after %lu implausible readings", w[0], (unsigned long)s_c.implausible);
        s_c.implausible = 0;
    }
    sensor_publish(s, "co2_ppm", w[0]);

    s_c.co2[s_c.i_co2] = w[0];
    s_c.i_co2 = (s_c.i_co2 + 1) % STABLE_WINDOW;
    if (s_c.n_co2 < STABLE_WINDOW) {
        s_c.n_co2++;
    }

    // difference to the reference temperature (SHT4x preferred; never ourselves)
    metric_t ref;
    if (metrics_get("sht4x.temperature_c", &ref) && now_us - ref.mono_us < 10 * 1000 * 1000) {
        if (s_c.diff_n >= OFFSET_AVG_N) {   // keep a sliding mean
            s_c.diff_sum -= s_c.diff_sum / s_c.diff_n;
            s_c.diff_n--;
        }
        s_c.diff_sum += t - ref.value;
        s_c.diff_n++;
    }

    // fresh air bookkeeping, NVS write at most hourly
    time_t now = utc_now();
    if (now) {
        if (w[0] < FRESH_PPM || s_c.fresh_utc == 0) {
            if (now - s_c.fresh_utc > 3600) {
                s_c.fresh_utc = now;
                nvs_save();
            }
        }
    }

    // fresh-air calibration session (BOOT long press)
    if (s_c.session) {
        int span;
        if (co2_stable(&span) && now_us - s_c.started_us >= FRC_MIN_RUN_US) {
            s_c.session = false;
            frc_checked(s, s_c.outdoor_ppm, false, s_c.session_msg, sizeof(s_c.session_msg));
            s_c.session_msg_until_us = now_us + 10LL * 60 * 1000 * 1000;
        } else if (now_us - s_c.session_start_us > SESSION_TIMEOUT_US) {
            s_c.session = false;
            snprintf(s_c.session_msg, sizeof(s_c.session_msg), "fresh-air cal. rejected: CO2 not stable in 30 min");
            s_c.session_msg_until_us = now_us + 10LL * 60 * 1000 * 1000;
            ESP_LOGW(TAG, "%s", s_c.session_msg);
        }
    }

    compensate_pressure(s, now_us);
    update_warn(s, now_us);
    return now_us + PERIOD_US;
}

static void detail(sensor_inst_t *s, char *buf, size_t len)
{
    metric_t m;
    size_t o = 0;
    if (metrics_get("scd41.co2_ppm", &m) || metrics_get("scd40.co2_ppm", &m)) {
        o += snprintf(buf + o, len - o, "CO2          %.0f ppm\n", m.value);
    }
    if (isnan(s_c.offset_c)) {
        o += snprintf(buf + o, len - o, "T offset     not calibrated\n");
    } else {
        o += snprintf(buf + o, len - o, "T offset     %.2f \xc2\xb0" "C\n", s_c.offset_c);
    }
    if (s_c.diff_n) {
        o += snprintf(buf + o, len - o, "T - SHT4x    %+.2f \xc2\xb0" "C\n", s_c.diff_sum / s_c.diff_n);
    }
    o += snprintf(buf + o, len - o, "Pressure     %u hPa (compensation)\n", s_c.pressure_hpa);
    o += snprintf(buf + o, len - o, "Outdoor CO2  %u ppm, ASC target %s\n", s_c.outdoor_ppm, s_c.asc_target_ok ? "set" : "FAILED");
    if (s_c.frc_utc) {
        char d[24];
        struct tm tm;
        time_t t = s_c.frc_utc;
        gmtime_r(&t, &tm);
        strftime(d, sizeof(d), "%Y-%m-%d %H:%M", &tm);
        o += snprintf(buf + o, len - o, "Calibrated   %s, %u ppm, %+d\n", d, s_c.frc_target, s_c.frc_correction);
    } else {
        o += snprintf(buf + o, len - o, "Calibrated   never (FRC)\n");
    }
    time_t now = utc_now();
    if (now && s_c.fresh_utc) {
        snprintf(buf + o, len - o, "Fresh air    %lld h ago (< %d ppm)", (long long)((now - s_c.fresh_utc) / 3600), FRESH_PPM);
    }
}

const sensor_driver_t sensor_drv_scd4x = {
    .driver = "scd4x",
    .addrs = k_addrs,
    .n_addrs = sizeof(k_addrs),
    .probe = probe,
    .poll = poll,
    .detail = detail,
};

// --- API (sensors.h) ---

static esp_err_t request(req_t r, float arg, char *msg, size_t len)
{
    if (!s_c.s) {
        snprintf(msg, len, "no SCD4x");
        return ESP_ERR_NOT_FOUND;
    }
    xSemaphoreTake(s_c.lock, portMAX_DELAY);
    xSemaphoreTake(s_c.done, 0);   // drop a stale signal
    s_c.req_arg = arg;
    s_c.req = r;
    sensors_kick(s_c.s);
    bool ok = xSemaphoreTake(s_c.done, pdMS_TO_TICKS(20000)) == pdTRUE;   // self test takes 10 s
    esp_err_t err = ok ? s_c.req_err : ESP_ERR_TIMEOUT;
    strlcpy(msg, ok ? s_c.req_msg : "timeout", len);
    xSemaphoreGive(s_c.lock);
    return err;
}

esp_err_t sensors_scd4x_offset_auto(char *msg, size_t len)
{
    return request(REQ_OFFSET_AUTO, 0, msg, len);
}

esp_err_t sensors_scd4x_offset_set(float c, char *msg, size_t len)
{
    return request(REQ_OFFSET_SET, c, msg, len);
}

esp_err_t sensors_scd4x_diag(char *msg, size_t len)
{
    return request(REQ_DIAG, 0, msg, len);
}

esp_err_t sensors_scd4x_offset_reset(char *msg, size_t len)
{
    return request(REQ_OFFSET_RESET, 0, msg, len);
}

esp_err_t sensors_scd4x_frc(uint16_t ppm, bool force, char *msg, size_t len)
{
    return request(force ? REQ_FRC_FORCE : REQ_FRC, ppm, msg, len);
}

esp_err_t sensors_scd4x_set_outdoor(uint16_t ppm)
{
    if (ppm < 380 || ppm > 500) {
        return ESP_ERR_INVALID_ARG;
    }
    s_c.outdoor_ppm = ppm;
    nvs_save();   // ASC target follows at the next boot (idle mode only)
    return ESP_OK;
}

esp_err_t sensors_scd4x_fresh_air_start(void)
{
    if (!s_c.s) {
        return ESP_ERR_NOT_FOUND;
    }
    s_c.session_start_us = esp_timer_get_time();
    s_c.session_msg[0] = '\0';
    s_c.session = true;
    ESP_LOGI(TAG, "fresh-air calibration started, target %u ppm, waiting for stable CO2", s_c.outdoor_ppm);
    sensors_kick(s_c.s);
    return ESP_OK;
}

bool sensors_scd4x_calibration_age(int *days)
{
    time_t now = utc_now();
    if (!s_c.s || !s_c.frc_utc || !now) {
        return false;
    }
    *days = (now - s_c.frc_utc) / 86400;
    return true;
}
