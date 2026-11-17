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
//   It also refuses a target more than FRC_MAX_DIFF_PPM away from the measured mean, so a
//   fresh-air calibration cannot run on indoor air. "force" skips all checks.
// - The fresh-air session counts only readings taken after the key press.
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
#define CMD_FACTORY_RESET    0x3632   // idle only, 1200 ms: EEPROM settings + FRC/ASC history erased
#define CMD_SET_ASC_ENABLED  0x2416   // idle only
#define CMD_GET_ASC_INITIAL  0x2340   // hours
#define CMD_GET_ASC_STANDARD 0x234B   // hours
#define CMD_GET_ALTITUDE     0x2322
#define CMD_SINGLE_SHOT      0x219D   // idle only, 5000 ms (SCD41/43)
#define CMD_POWER_DOWN       0x36E0   // idle -> sleep, 1 ms
#define CMD_WAKE_UP          0x36F6   // sleep -> idle, 30 ms, NOT acknowledged by the sensor
// command codes and durations: Sensirion SCD4x datasheet v1.5 (July 2023), table 9

#define PERIOD_US            (5LL * 1000 * 1000)
#define PRESSURE_EVERY_US    (60LL * 1000 * 1000)
#define FRC_MIN_RUN_US       (3LL * 60 * 1000 * 1000)   // Sensirion: >= 3 min periodic operation
#define STABLE_WINDOW        36                          // samples = 3 min at 5 s
#define STABLE_MAX_SPAN_PPM  40                          // ASSUMPTION: our threshold for "constant"
#define FRC_MAX_DIFF_PPM     150                         // ASSUMPTION: our limit, Sensirion gives none
#define SESSION_TIMEOUT_US   (30LL * 60 * 1000 * 1000)   // fresh-air session gives up after 30 min
#define FRESH_PPM            450
#define CO2_PLAUSIBLE_MIN    300   // below any real air (outdoor ~430): sensor problem, not data
#define FRESH_WARN_H         156
#define OFFSET_AVG_N         24                          // 2 min of differences for "offset auto"
#define NVS_NS               "scd4x"
#define HEALTH_DEFAULT_MIN   30    // self test interval, 'scd41 health <min>' (0 = only on faults)
#define HEALTH_FAULT_GAP_US  (10LL * 60 * 1000 * 1000)   // fault-triggered checks at most every 10 min
#define HEALTH_LOG_N         16

static const uint8_t k_addrs[] = {0x62};

typedef enum {
    REQ_NONE,
    REQ_OFFSET_AUTO,
    REQ_OFFSET_SET,
    REQ_OFFSET_RESET,
    REQ_FRC,
    REQ_FRC_FORCE,
    REQ_DIAG,
    REQ_SINGLE,
    REQ_REINIT,
    REQ_SLEEP_CYCLE,
    REQ_ASC,
    REQ_FACTORY_RESET,
} req_t;

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
    char req_msg[SCD4X_MSG_LEN];
    // fresh-air session (BOOT long press); started in the sensors task, see poll()
    volatile bool session_req;
    bool session;
    int64_t session_start_us;
    char session_msg[80];           // = sizeof(sensor_inst_t.warn), shown on the overview
    int64_t session_msg_until_us;
    bool asc_target_ok;
    uint32_t implausible;           // consecutive readings below CO2_PLAUSIBLE_MIN
    bool log_raw;                   // 'scd41 log on': print every raw reading
    uint32_t good_streak;           // consecutive plausible CO2 readings
    uint32_t faults_total;          // implausible CO2 readings since boot
    // health check: self test, on failure power_down/wake_up and a second self test
    uint16_t health_min;            // interval in minutes, 0 = only after implausible readings
    int health_state;               // see health_step()
    bool health_by_fault;
    int64_t health_next_us;
    int64_t health_last_fault_check_us;
    uint16_t health_st1;
    struct {
        time_t utc;
        int64_t mono_us;
        bool by_fault;
        uint16_t st1, st2;          // self test results; st2 only after a sleep cycle
        bool cycled;
    } hlog[HEALTH_LOG_N];
    int hlog_n, hlog_i;
    uint32_t h_tests, h_fails, h_recovered;
    uint16_t last_ready, last_raw[3];
    uint32_t n_readings;
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
    s_c.health_min = HEALTH_DEFAULT_MIN;
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
    if (nvs_get_u16(h, "health_min", &u16) == ESP_OK) {
        s_c.health_min = u16;
    }
    nvs_get_u32(h, "h_tests", &s_c.h_tests);
    nvs_get_u32(h, "h_fails", &s_c.h_fails);
    nvs_get_u32(h, "h_recov", &s_c.h_recovered);
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
    nvs_set_u16(h, "health_min", s_c.health_min);
    nvs_set_u32(h, "h_tests", s_c.h_tests);
    nvs_set_u32(h, "h_fails", s_c.h_fails);
    nvs_set_u32(h, "h_recov", s_c.h_recovered);
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

// mean of the readings in the stability window (filled from index 0 after a reset)
static bool co2_mean(float *mean)
{
    if (s_c.n_co2 == 0) {
        return false;
    }
    uint32_t sum = 0;
    for (int i = 0; i < s_c.n_co2; i++) {
        sum += s_c.co2[i];
    }
    *mean = (float)sum / s_c.n_co2;
    return true;
}

// FRC with checks; runs in the sensors task
static esp_err_t frc_checked(sensor_inst_t *s, uint16_t target, bool force, char *msg, size_t len)
{
    int64_t now = esp_timer_get_time();
    int span;
    bool stable = co2_stable(&span);
    float mean;
    // first, so it also applies to a partly filled window; the checks below still require 3 min
    if (!force && co2_mean(&mean) && fabsf(target - mean) > FRC_MAX_DIFF_PPM) {
        snprintf(msg, len, "rejected: %.0f ppm measured, target %u ppm (max diff %d)", mean, target,
                 FRC_MAX_DIFF_PPM);
        return ESP_ERR_INVALID_ARG;
    }
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

// after reinit / wake_up / factory reset: our runtime settings (sensor in idle mode)
static void reapply_settings(sensor_inst_t *s)
{
    if (!isnan(s_c.offset_c)) {
        write_offset(s, s_c.offset_c);
    }
    s_c.asc_target_ok = send_word(s, CMD_SET_ASC_TARGET, s_c.outdoor_ppm) == ESP_OK;
    vTaskDelay(pdMS_TO_TICKS(2));
    s_c.last_pressure_us = -PRESSURE_EVERY_US;   // pressure compensation again right away
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
        // read-only: settings and the built-in self test; nothing changes the calibration
        uint16_t sn[3] = {0}, var = 0xffff, asc_en = 0xffff, asc_tgt = 0xffff, asc_ini = 0xffff, asc_std = 0xffff;
        uint16_t alt = 0xffff, pres = 0xffff, st = 0xffff;
        float off = NAN;
        stop_periodic(s);
        esp_err_t e_sn = read_words(s, CMD_SERIAL, sn, 3);
        read_words(s, CMD_VARIANT, &var, 1);
        esp_err_t e_asc = read_words(s, CMD_GET_ASC_ENABLED, &asc_en, 1);
        read_words(s, CMD_GET_ASC_TARGET, &asc_tgt, 1);
        read_words(s, CMD_GET_ASC_INITIAL, &asc_ini, 1);
        read_words(s, CMD_GET_ASC_STANDARD, &asc_std, 1);
        read_words(s, CMD_GET_ALTITUDE, &alt, 1);
        read_words(s, CMD_SET_PRESSURE, &pres, 1);    // get_ambient_pressure uses the same code
        read_offset(s, &off);
        esp_err_t e_st = send(s, CMD_SELF_TEST);
        vTaskDelay(pdMS_TO_TICKS(10500));
        if (e_st == ESP_OK) {
            e_st = receive_words(s, &st, 1);
        }
        start_periodic(s);
        snprintf(msg, len,
                 "serial      %04x%04x%04x (%s)\n"
                 "variant     0x%04x\n"
                 "ASC         enabled %u (%s), target %u ppm, initial %u h, standard %u h\n"
                 "T offset    %.2f C\n"
                 "altitude    %u m, ambient pressure %u hPa\n"
                 "last raw    ready 0x%04x, co2 %u, t %u, rh %u (%lu readings since boot)\n"
                 "self test   %s: 0x%04x %s",
                 sn[0], sn[1], sn[2], esp_err_to_name(e_sn), var, asc_en, esp_err_to_name(e_asc), asc_tgt, asc_ini,
                 asc_std, off, alt, pres, s_c.last_ready, s_c.last_raw[0], s_c.last_raw[1], s_c.last_raw[2],
                 (unsigned long)s_c.n_readings, esp_err_to_name(e_st), st,
                 st == 0 ? "= no malfunction"
                         : "= MALFUNCTION (Sensirion: sensor malfunction or supply voltage/current unstable or too low)");
        err = e_st;
        goto out;
    }
    if (r == REQ_SINGLE) {
        // one single-shot measurement in idle mode (SCD41/43), independent of periodic mode
        uint16_t w[3] = {0};
        stop_periodic(s);
        err = send(s, CMD_SINGLE_SHOT);
        vTaskDelay(pdMS_TO_TICKS(5100));
        if (err == ESP_OK) {
            err = read_words(s, CMD_READ_MEASUREMENT, w, 3);
        }
        start_periodic(s);
        snprintf(msg, len, "single shot: %s, co2 %u ppm, t %.2f C, rh %.1f %% (raw 0x%04x 0x%04x 0x%04x)",
                 esp_err_to_name(err), w[0], -45.0f + 175.0f * w[1] / 65536.0f, 100.0f * w[2] / 65536.0f, w[0], w[1],
                 w[2]);
        goto out;
    }
    if (r == REQ_REINIT || r == REQ_SLEEP_CYCLE) {
        stop_periodic(s);
        if (r == REQ_REINIT) {
            err = send(s, CMD_REINIT);   // reload user settings from EEPROM
            vTaskDelay(pdMS_TO_TICKS(30));
        } else {
            err = send(s, CMD_POWER_DOWN);
            vTaskDelay(pdMS_TO_TICKS(1000));
            send(s, CMD_WAKE_UP);        // not acknowledged by design (datasheet 3.10.4)
            vTaskDelay(pdMS_TO_TICKS(30));
        }
        uint16_t sn[3];
        esp_err_t e2 = read_words(s, CMD_SERIAL, sn, 3);
        reapply_settings(s);
        start_periodic(s);
        snprintf(msg, len, "%s: %s, sensor answers afterwards: %s; offset and ASC target re-applied",
                 r == REQ_REINIT ? "reinit" : "power_down/wake_up", esp_err_to_name(err), esp_err_to_name(e2));
        if (err == ESP_OK) {
            err = e2;
        }
        goto out;
    }
    if (r == REQ_ASC) {
        stop_periodic(s);
        err = send_word(s, CMD_SET_ASC_ENABLED, s_c.req_arg != 0);
        vTaskDelay(pdMS_TO_TICKS(2));
        start_periodic(s);
        snprintf(msg, len, "ASC %s: %s (RAM only, back to the EEPROM value after a power cycle)",
                 s_c.req_arg != 0 ? "enabled" : "disabled", esp_err_to_name(err));
        goto out;
    }
    if (r == REQ_FACTORY_RESET) {
        stop_periodic(s);
        err = send(s, CMD_FACTORY_RESET);
        vTaskDelay(pdMS_TO_TICKS(1300));
        uint16_t sn[3];
        esp_err_t e2 = read_words(s, CMD_SERIAL, sn, 3);
        // the sensor forgot every FRC and its ASC history: forget our record and offset too
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_key(h, "offset");
            nvs_erase_key(h, "frc_utc");
            nvs_erase_key(h, "frc_tgt");
            nvs_erase_key(h, "frc_corr");
            nvs_commit(h);
            nvs_close(h);
        }
        s_c.offset_c = NAN;
        s_c.frc_utc = 0;
        s_c.frc_target = 0;
        s_c.frc_correction = 0;
        s_c.diff_sum = 0;
        s_c.diff_n = 0;
        float cur = NAN;
        read_offset(s, &cur);
        reapply_settings(s);   // ASC target = outdoor value
        start_periodic(s);
        ESP_LOGW(TAG, "factory reset: %s", esp_err_to_name(err));
        snprintf(msg, len,
                 "factory reset: %s, sensor answers: %s, EEPROM offset now %.2f C; FRC/ASC history erased, "
                 "our FRC record and offset cleared",
                 esp_err_to_name(err), esp_err_to_name(e2), cur);
        if (err == ESP_OK) {
            err = e2;
        }
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

// --- health check (non-blocking: the sensors task keeps serving the other sensors) ---

enum { H_IDLE, H_STOPPING, H_TEST1, H_SLEEPING, H_WAKING, H_TEST2 };

static void health_log(bool cycled, uint16_t st2)
{
    int i = s_c.hlog_i;
    s_c.hlog[i].utc = utc_now();
    s_c.hlog[i].mono_us = esp_timer_get_time();
    s_c.hlog[i].by_fault = s_c.health_by_fault;
    s_c.hlog[i].st1 = s_c.health_st1;
    s_c.hlog[i].st2 = st2;
    s_c.hlog[i].cycled = cycled;
    s_c.hlog_i = (i + 1) % HEALTH_LOG_N;
    if (s_c.hlog_n < HEALTH_LOG_N) {
        s_c.hlog_n++;
    }
    s_c.h_tests++;
    if (s_c.health_st1 != 0) {
        s_c.h_fails++;
        if (cycled && st2 == 0) {
            s_c.h_recovered++;
        }
    }
    nvs_save();
    if (s_c.health_st1 == 0) {
        ESP_LOGI(TAG, "health check (%s): self test ok", s_c.health_by_fault ? "after fault" : "timer");
    } else {
        ESP_LOGW(TAG, "health check (%s): self test 0x%04x, after power_down/wake_up 0x%04x - %s",
                 s_c.health_by_fault ? "after fault" : "timer", s_c.health_st1, st2,
                 st2 == 0 ? "recovered" : "STILL FAILING");
    }
}

static void health_schedule(int64_t now_us)
{
    s_c.health_next_us = s_c.health_min ? now_us + (int64_t)s_c.health_min * 60 * 1000000 : INT64_MAX;
}

// one step; returns the time of the next poll
static int64_t health_step(sensor_inst_t *s, int64_t now_us)
{
    uint16_t st;
    switch (s_c.health_state) {
    case H_STOPPING:   // stop_periodic sent 500 ms ago
        send(s, CMD_SELF_TEST);
        s_c.health_state = H_TEST1;
        return now_us + 10500 * 1000LL;
    case H_TEST1:
        if (receive_words(s, &st, 1) != ESP_OK) {
            st = 0xffff;
        }
        s_c.health_st1 = st;
        if (st == 0) {
            health_log(false, 0);
            break;
        }
        send(s, CMD_POWER_DOWN);
        s_c.health_state = H_SLEEPING;
        return now_us + 1000 * 1000LL;
    case H_SLEEPING:
        send(s, CMD_WAKE_UP);   // not acknowledged by design
        s_c.health_state = H_WAKING;
        return now_us + 30 * 1000LL;
    case H_WAKING:
        reapply_settings(s);
        send(s, CMD_SELF_TEST);
        s_c.health_state = H_TEST2;
        return now_us + 10500 * 1000LL;
    case H_TEST2:
        if (receive_words(s, &st, 1) != ESP_OK) {
            st = 0xffff;
        }
        health_log(true, st);
        break;
    default:
        break;
    }
    s_c.health_state = H_IDLE;
    start_periodic(s);
    health_schedule(now_us);
    return now_us + PERIOD_US;
}

static int64_t health_start(sensor_inst_t *s, int64_t now_us, bool by_fault)
{
    s_c.health_by_fault = by_fault;
    if (by_fault) {
        s_c.health_last_fault_check_us = now_us;
    }
    send(s, CMD_STOP_PERIODIC);
    s_c.health_state = H_STOPPING;
    return now_us + 500 * 1000LL;
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
    health_schedule(esp_timer_get_time());
    s_c.health_last_fault_check_us = -HEALTH_FAULT_GAP_US;
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
        if (span < 0) {
            snprintf(w, sizeof(w), "fresh-air cal.: %d:%02d, readings %d/%d", el / 60, el % 60, s_c.n_co2,
                     STABLE_WINDOW);
        } else {
            snprintf(w, sizeof(w), "fresh-air cal.: %d:%02d, span %d ppm", el / 60, el % 60, span);
        }
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
    if (s_c.health_state != H_IDLE) {
        return health_step(s, now_us);
    }
    handle_request(s);
    if (now_us >= s_c.health_next_us) {
        return health_start(s, now_us, false);
    }

    if (s_c.session_req) {
        // Only readings from now on count: the window may hold minutes of stable indoor air,
        // and the device may be carried outside only after the key press.
        s_c.session_req = false;
        s_c.n_co2 = s_c.i_co2 = 0;
        s_c.session_start_us = now_us;
        s_c.session_msg[0] = '\0';
        s_c.session = true;
        ESP_LOGI(TAG, "fresh-air calibration started, target %u ppm: needs %d stable readings (3 min) from now",
                 s_c.outdoor_ppm, STABLE_WINDOW);
    }

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
    s_c.last_ready = ready;
    memcpy(s_c.last_raw, w, sizeof(w));
    s_c.n_readings++;
    if (s_c.log_raw) {
        printf("scd41 raw: ready 0x%04x co2 %u t 0x%04x rh 0x%04x\n", ready, w[0], w[1], w[2]);
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
        s_c.good_streak = 0;
        s_c.faults_total++;
        s_c.n_co2 = s_c.i_co2 = 0;
        compensate_pressure(s, now_us);
        update_warn(s, now_us);
        if (now_us - s_c.health_last_fault_check_us >= HEALTH_FAULT_GAP_US) {
            return health_start(s, now_us, true);   // self test right away, sleep cycle if it fails
        }
        return now_us + PERIOD_US;
    }
    if (s_c.implausible) {
        ESP_LOGI(TAG, "CO2 plausible again (%u ppm) after %lu implausible readings", w[0], (unsigned long)s_c.implausible);
        s_c.implausible = 0;
    }
    sensor_publish(s, "co2_ppm", w[0]);
    s_c.good_streak++;

    s_c.co2[s_c.i_co2] = w[0];
    s_c.i_co2 = (s_c.i_co2 + 1) % STABLE_WINDOW;
    if (s_c.n_co2 < STABLE_WINDOW) {
        s_c.n_co2++;
    }

    // difference to the reference temperature: the primary one if it comes from an SHT4x (any
    // address). Not a BME6xx (self-heating) and never ourselves.
    metric_t ref;
    if (metrics_get_primary("temperature_c", &ref) && strncmp(ref.key, "sht4", 4) == 0 &&
        now_us - ref.mono_us < 10 * 1000 * 1000) {
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
        if (co2_stable(&span) && now_us - s_c.session_start_us >= FRC_MIN_RUN_US) {
            s_c.session = false;
            char m[sizeof(s_c.session_msg) - sizeof("fresh-air: ") + 1];
            frc_checked(s, s_c.outdoor_ppm, false, m, sizeof(m));
            snprintf(s_c.session_msg, sizeof(s_c.session_msg), "fresh-air: %s", m);
            ESP_LOGW(TAG, "%s", s_c.session_msg);
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
    char key[METRIC_KEY_LEN];
    snprintf(key, sizeof(key), "%s.co2_ppm", s->name);
    if (metrics_get(key, &m)) {
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
    bool ok = xSemaphoreTake(s_c.done, pdMS_TO_TICKS(40000)) == pdTRUE;   // self test 10 s, health check up to 23 s first
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

esp_err_t sensors_scd4x_single_shot(char *msg, size_t len)
{
    return request(REQ_SINGLE, 0, msg, len);
}

esp_err_t sensors_scd4x_reinit(bool sleep_cycle, char *msg, size_t len)
{
    return request(sleep_cycle ? REQ_SLEEP_CYCLE : REQ_REINIT, 0, msg, len);
}

esp_err_t sensors_scd4x_asc(bool on, char *msg, size_t len)
{
    return request(REQ_ASC, on ? 1 : 0, msg, len);
}

esp_err_t sensors_scd4x_factory_reset(char *msg, size_t len)
{
    return request(REQ_FACTORY_RESET, 0, msg, len);
}

void sensors_scd4x_log_raw(bool on)
{
    s_c.log_raw = on;
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
    s_c.session_req = true;   // the sensors task resets the window and starts the session
    sensors_kick(s_c.s);
    return ESP_OK;
}

esp_err_t sensors_scd4x_health_interval(int minutes)
{
    if (!s_c.s || minutes < 0 || minutes > 1440) {
        return ESP_ERR_INVALID_ARG;
    }
    s_c.health_min = minutes;
    health_schedule(esp_timer_get_time());
    nvs_save();
    return ESP_OK;
}

void sensors_scd4x_health_print(void)
{
    if (!s_c.s) {
        printf("no SCD4x\n");
        return;
    }
    printf("interval %u min (%s), after implausible readings at most every %lld min\n", s_c.health_min,
           s_c.health_min ? "self test, power_down/wake_up on failure" : "timer off",
           (long long)(HEALTH_FAULT_GAP_US / 60000000));
    printf("since first use: %lu tests, %lu failed, %lu recovered by power_down/wake_up\n", (unsigned long)s_c.h_tests,
           (unsigned long)s_c.h_fails, (unsigned long)s_c.h_recovered);
    if (s_c.health_next_us != INT64_MAX) {
        printf("next check in %lld s\n", (long long)((s_c.health_next_us - esp_timer_get_time()) / 1000000));
    }
    for (int k = 0; k < s_c.hlog_n; k++) {
        int i = (s_c.hlog_i - s_c.hlog_n + k + HEALTH_LOG_N) % HEALTH_LOG_N;
        char t[24] = "clock not set";
        if (s_c.hlog[i].utc) {
            struct tm tm;
            localtime_r(&s_c.hlog[i].utc, &tm);
            strftime(t, sizeof(t), "%Y-%m-%d %H:%M:%S", &tm);
        }
        printf("  %s %-6s self test 0x%04x", t, s_c.hlog[i].by_fault ? "fault" : "timer", s_c.hlog[i].st1);
        if (s_c.hlog[i].cycled) {
            printf(", sleep cycle, again 0x%04x %s", s_c.hlog[i].st2, s_c.hlog[i].st2 == 0 ? "recovered" : "STILL FAILING");
        }
        printf("\n");
    }
}

bool sensors_scd4x_co2_state(scd4x_co2_state_t *st)
{
    if (!s_c.s) {
        return false;
    }
    st->good_streak = s_c.good_streak;
    st->readings = s_c.n_readings;
    st->faults = s_c.faults_total;
    return true;
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
