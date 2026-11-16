// Bosch BME680 / BME688 / BME690 detection; BME680 runs with BSEC2 (IAQ), BME688 raw.
// Chip ID 0xD0 = 0x61 for all three; variant ID 0xF0: 0x00 BME680, 0x01 BME688, 0x02 BME690
// (BME690_SensorAPI bme69x_defs.h, BME68x_SensorAPI bme68x_defs.h).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bme68x.h"
#include "bsec_interface.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sensor_driver.h"

static const char *TAG = "bme68x";

#define REG_CHIP_ID     0xD0
#define REG_VARIANT_ID  0xF0
#define CHIP_ID         0x61
#define VARIANT_BME680  0x00
#define VARIANT_BME688  0x01
#define VARIANT_BME690  0x02

#define RAW_PERIOD_US       (3LL * 1000 * 1000)
#define STATE_SAVE_US       (4LL * 3600 * 1000 * 1000)   // BSEC learns over days, keep it
#define NVS_NS              "bsec"
#define NVS_KEY             "state"

static const uint8_t k_addrs[] = {0x76, 0x77};

static const uint8_t k_bsec_config[] = {
#include "bsec_iaq.txt"
};

typedef struct {
    struct bme68x_dev dev;
    struct bme68x_conf conf;
    struct bme68x_heatr_conf heatr;
    bool bsec;                 // false = raw mode
    int64_t last_state_save_us;
    uint8_t iaq_accuracy;
    char bsec_version[16];
} ctx_t;

// --- BME68x SensorAPI glue ---

static BME68X_INTF_RET_TYPE api_read(uint8_t reg, uint8_t *data, uint32_t len, void *intf)
{
    return sensor_read_reg((sensor_inst_t *)intf, reg, data, len) == ESP_OK ? BME68X_OK : BME68X_E_COM_FAIL;
}

static BME68X_INTF_RET_TYPE api_write(uint8_t reg, const uint8_t *data, uint32_t len, void *intf)
{
    // the API writes register/value pairs: reg, d0, r1, d1 ... - one transaction
    return sensor_write_reg((sensor_inst_t *)intf, reg, data, len) == ESP_OK ? BME68X_OK : BME68X_E_COM_FAIL;
}

static void api_delay(uint32_t us, void *intf)
{
    if (us >= 2000) {
        vTaskDelay(pdMS_TO_TICKS((us + 999) / 1000));
    } else {
        esp_rom_delay_us(us);
    }
}

// --- BSEC state in NVS ---

static void state_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t state[BSEC_MAX_STATE_BLOB_SIZE];
    size_t len = sizeof(state);
    if (nvs_get_blob(h, NVS_KEY, state, &len) == ESP_OK) {
        uint8_t *work = malloc(BSEC_MAX_WORKBUFFER_SIZE);
        if (work) {
            bsec_library_return_t r = bsec_set_state(state, len, work, BSEC_MAX_WORKBUFFER_SIZE);
            ESP_LOGI(TAG, "BSEC state restored (%u bytes): %d", (unsigned)len, r);
            free(work);
        }
    }
    nvs_close(h);
}

static void save_state(sensor_inst_t *s)
{
    ctx_t *c = s->ctx;
    if (!c || !c->bsec) {
        return;
    }
    uint8_t state[BSEC_MAX_STATE_BLOB_SIZE];
    uint32_t len = 0;
    uint8_t *work = malloc(BSEC_MAX_WORKBUFFER_SIZE);
    if (!work) {
        return;
    }
    bsec_library_return_t r = bsec_get_state(0, state, sizeof(state), work, BSEC_MAX_WORKBUFFER_SIZE, &len);
    free(work);
    if (r != BSEC_OK) {
        ESP_LOGW(TAG, "bsec_get_state: %d", r);
        return;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, NVS_KEY, state, len);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "BSEC state saved (%u bytes)", (unsigned)len);
    }
    c->last_state_save_us = esp_timer_get_time();
}

static bool bsec_setup(ctx_t *c)
{
    bsec_version_t v;
    if (bsec_init() != BSEC_OK) {
        return false;
    }
    bsec_get_version(&v);
    snprintf(c->bsec_version, sizeof(c->bsec_version), "%d.%d.%d.%d", v.major, v.minor, v.major_bugfix,
             v.minor_bugfix);

    uint8_t *work = malloc(BSEC_MAX_WORKBUFFER_SIZE);
    if (!work) {
        return false;
    }
    bsec_library_return_t r = bsec_set_configuration(k_bsec_config, sizeof(k_bsec_config), work,
                                                     BSEC_MAX_WORKBUFFER_SIZE);
    free(work);
    if (r != BSEC_OK) {
        ESP_LOGE(TAG, "bsec_set_configuration: %d", r);
        return false;
    }
    state_load();

    static const uint8_t outs[] = {
        BSEC_OUTPUT_IAQ,
        BSEC_OUTPUT_STATIC_IAQ,
        BSEC_OUTPUT_CO2_EQUIVALENT,
        BSEC_OUTPUT_BREATH_VOC_EQUIVALENT,
        BSEC_OUTPUT_RAW_TEMPERATURE,
        BSEC_OUTPUT_RAW_PRESSURE,
        BSEC_OUTPUT_RAW_HUMIDITY,
        BSEC_OUTPUT_RAW_GAS,
        BSEC_OUTPUT_STABILIZATION_STATUS,
        BSEC_OUTPUT_RUN_IN_STATUS,
        BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE,
        BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY,
    };
    bsec_sensor_configuration_t req[sizeof(outs)];
    for (size_t i = 0; i < sizeof(outs); i++) {
        req[i].sensor_id = outs[i];
        req[i].sample_rate = BSEC_SAMPLE_RATE_LP;   // must match the 3 s config blob
    }
    bsec_sensor_configuration_t required[BSEC_MAX_PHYSICAL_SENSOR];
    uint8_t n_required = BSEC_MAX_PHYSICAL_SENSOR;
    r = bsec_update_subscription(req, sizeof(outs), required, &n_required);
    if (r != BSEC_OK) {
        ESP_LOGE(TAG, "bsec_update_subscription: %d", r);
        return false;
    }
    ESP_LOGI(TAG, "BSEC %s ready, IAQ LP mode (3 s)", c->bsec_version);
    return true;
}

// --- driver ---

static esp_err_t probe(sensor_inst_t *s)
{
    uint8_t id = 0, variant = 0xff;
    if (sensor_read_reg(s, REG_CHIP_ID, &id, 1) != ESP_OK || id != CHIP_ID) {
        return ESP_ERR_NOT_FOUND;
    }
    sensor_read_reg(s, REG_VARIANT_ID, &variant, 1);

    const char *model;
    switch (variant) {
    case VARIANT_BME680:
        model = "BME680";
        break;
    case VARIANT_BME688:
        model = "BME688";
        break;
    case VARIANT_BME690:
        model = "BME690";
        break;
    default:
        model = "BME68x?";
        break;
    }
    strlcpy(s->model, model, sizeof(s->model));
    snprintf(s->name, sizeof(s->name), "%c%c%c%c%c%c", model[0] | 0x20, model[1] | 0x20, model[2] | 0x20,
             model[3], model[4], model[5]);   // "bme680"

    if (variant == VARIANT_BME690) {
        // different compensation and BSEC; needs the BME690 SensorAPI - not built in yet
        s->supported = false;
        strlcpy(s->info, "detected, no driver yet", sizeof(s->info));
        return ESP_OK;
    }
    if (variant != VARIANT_BME680 && variant != VARIANT_BME688) {
        s->supported = false;
        snprintf(s->info, sizeof(s->info), "unknown variant 0x%02x", variant);
        return ESP_OK;
    }

    ctx_t *c = calloc(1, sizeof(ctx_t));
    if (!c) {
        return ESP_ERR_NO_MEM;
    }
    s->ctx = c;
    c->dev.intf = BME68X_I2C_INTF;
    c->dev.intf_ptr = s;
    c->dev.read = api_read;
    c->dev.write = api_write;
    c->dev.delay_us = api_delay;
    c->dev.amb_temp = 22;
    int8_t rslt = bme68x_init(&c->dev);
    if (rslt != BME68X_OK) {
        ESP_LOGE(TAG, "bme68x_init: %d", rslt);
        return ESP_FAIL;
    }
    // BSEC config blob is for the BME680; BME688 IAQ would need another blob
    c->bsec = variant == VARIANT_BME680 && bsec_setup(c);
    if (!c->bsec) {
        c->conf.os_hum = BME68X_OS_2X;
        c->conf.os_temp = BME68X_OS_8X;
        c->conf.os_pres = BME68X_OS_4X;
        c->conf.filter = BME68X_FILTER_OFF;
        c->conf.odr = BME68X_ODR_NONE;
        c->heatr.enable = BME68X_ENABLE;
        c->heatr.heatr_temp = 320;
        c->heatr.heatr_dur = 150;
        strlcpy(s->info, "raw mode (no BSEC)", sizeof(s->info));
    }
    c->last_state_save_us = esp_timer_get_time();
    s->supported = true;
    return ESP_OK;
}

// one forced measurement with the current c->conf / c->heatr
static int8_t measure(ctx_t *c, struct bme68x_data *d, uint8_t *n_fields)
{
    int8_t r = bme68x_set_conf(&c->conf, &c->dev);
    if (r == BME68X_OK) {
        r = bme68x_set_heatr_conf(BME68X_FORCED_MODE, &c->heatr, &c->dev);
    }
    if (r == BME68X_OK) {
        r = bme68x_set_op_mode(BME68X_FORCED_MODE, &c->dev);
    }
    if (r != BME68X_OK) {
        return r;
    }
    uint32_t dur_us = bme68x_get_meas_dur(BME68X_FORCED_MODE, &c->conf, &c->dev) + c->heatr.heatr_dur * 1000;
    c->dev.delay_us(dur_us, c->dev.intf_ptr);
    return bme68x_get_data(BME68X_FORCED_MODE, d, n_fields, &c->dev);
}

static int64_t poll_raw(sensor_inst_t *s, ctx_t *c, int64_t now_us)
{
    struct bme68x_data d;
    uint8_t n = 0;
    if (measure(c, &d, &n) != BME68X_OK || n == 0) {
        return -1;
    }
    sensor_publish(s, "temperature_c", d.temperature);
    sensor_publish(s, "humidity_pct", d.humidity);
    sensor_publish(s, "pressure_hpa", d.pressure / 100.0f);
    if ((d.status & BME68X_GASM_VALID_MSK) && (d.status & BME68X_HEAT_STAB_MSK)) {
        sensor_publish(s, "gas_ohm", d.gas_resistance);
    }
    return now_us + RAW_PERIOD_US;
}

static int64_t poll_bsec(sensor_inst_t *s, ctx_t *c, int64_t now_us)
{
    int64_t ts_ns = now_us * 1000;
    bsec_bme_settings_t st;
    bsec_library_return_t r = bsec_sensor_control(ts_ns, &st);
    if (r < 0) {
        ESP_LOGW(TAG, "bsec_sensor_control: %d", r);
        return -1;
    }
    if (r > 0) {
        ESP_LOGD(TAG, "bsec_sensor_control warning %d", r);
    }

    if (st.trigger_measurement && st.op_mode == BME68X_FORCED_MODE) {
        c->conf.os_hum = st.humidity_oversampling;
        c->conf.os_temp = st.temperature_oversampling;
        c->conf.os_pres = st.pressure_oversampling;
        c->conf.filter = BME68X_FILTER_OFF;
        c->conf.odr = BME68X_ODR_NONE;
        c->heatr.enable = st.run_gas ? BME68X_ENABLE : BME68X_DISABLE;
        c->heatr.heatr_temp = st.heater_temperature;
        c->heatr.heatr_dur = st.heater_duration;

        struct bme68x_data d;
        uint8_t n = 0;
        if (measure(c, &d, &n) != BME68X_OK) {
            return -1;
        }
        if (n > 0 && (d.status & BME68X_NEW_DATA_MSK)) {
            bsec_input_t in[5];
            uint8_t ni = 0;
            // heat source: self-heating of the board; 0 until we calibrate against an SHT4x
            in[ni++] = (bsec_input_t){.sensor_id = BSEC_INPUT_HEATSOURCE, .signal = 0.0f, .time_stamp = ts_ns};
            if (st.process_data & BSEC_PROCESS_TEMPERATURE) {
                in[ni++] = (bsec_input_t){.sensor_id = BSEC_INPUT_TEMPERATURE, .signal = d.temperature, .time_stamp = ts_ns};
            }
            if (st.process_data & BSEC_PROCESS_HUMIDITY) {
                in[ni++] = (bsec_input_t){.sensor_id = BSEC_INPUT_HUMIDITY, .signal = d.humidity, .time_stamp = ts_ns};
            }
            if (st.process_data & BSEC_PROCESS_PRESSURE) {
                in[ni++] = (bsec_input_t){.sensor_id = BSEC_INPUT_PRESSURE, .signal = d.pressure, .time_stamp = ts_ns};
            }
            if ((st.process_data & BSEC_PROCESS_GAS) && (d.status & BME68X_GASM_VALID_MSK)) {
                in[ni++] = (bsec_input_t){.sensor_id = BSEC_INPUT_GASRESISTOR, .signal = d.gas_resistance, .time_stamp = ts_ns};
            }
            for (int i = 0; i < ni; i++) {
                in[i].signal_dimensions = 1;
            }

            bsec_output_t out[BSEC_NUMBER_OUTPUTS];
            uint8_t no = BSEC_NUMBER_OUTPUTS;
            r = bsec_do_steps(in, ni, out, &no);
            if (r < 0) {
                ESP_LOGW(TAG, "bsec_do_steps: %d", r);
                return -1;
            }
            for (int i = 0; i < no; i++) {
                float v = out[i].signal;
                switch (out[i].sensor_id) {
                case BSEC_OUTPUT_IAQ:
                    sensor_publish(s, "iaq", v);
                    sensor_publish(s, "iaq_accuracy", out[i].accuracy);
                    c->iaq_accuracy = out[i].accuracy;
                    break;
                case BSEC_OUTPUT_STATIC_IAQ:
                    sensor_publish(s, "static_iaq", v);
                    break;
                case BSEC_OUTPUT_CO2_EQUIVALENT:
                    sensor_publish(s, "co2eq_ppm", v);
                    break;
                case BSEC_OUTPUT_BREATH_VOC_EQUIVALENT:
                    sensor_publish(s, "bvoc_ppm", v);
                    break;
                case BSEC_OUTPUT_RAW_TEMPERATURE:
                    sensor_publish(s, "raw_temperature_c", v);
                    break;
                case BSEC_OUTPUT_RAW_PRESSURE:
                    sensor_publish(s, "pressure_hpa", v / 100.0f);
                    break;
                case BSEC_OUTPUT_RAW_HUMIDITY:
                    sensor_publish(s, "raw_humidity_pct", v);
                    break;
                case BSEC_OUTPUT_RAW_GAS:
                    sensor_publish(s, "gas_ohm", v);
                    break;
                case BSEC_OUTPUT_STABILIZATION_STATUS:
                    sensor_publish(s, "stab_status", v);
                    break;
                case BSEC_OUTPUT_RUN_IN_STATUS:
                    sensor_publish(s, "runin_status", v);
                    break;
                case BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE:
                    sensor_publish(s, "temperature_c", v);
                    break;
                case BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY:
                    sensor_publish(s, "humidity_pct", v);
                    break;
                default:
                    break;
                }
            }
        }
    }
    snprintf(s->info, sizeof(s->info), "BSEC %s, IAQ accuracy %u", c->bsec_version, c->iaq_accuracy);

    if (now_us - c->last_state_save_us > STATE_SAVE_US) {
        save_state(s);
    }
    return st.next_call / 1000;
}

static int64_t poll(sensor_inst_t *s, int64_t now_us)
{
    ctx_t *c = s->ctx;
    return c->bsec ? poll_bsec(s, c, now_us) : poll_raw(s, c, now_us);
}

const sensor_driver_t sensor_drv_bme68x = {
    .driver = "bme68x",
    .addrs = k_addrs,
    .n_addrs = sizeof(k_addrs),
    .probe = probe,
    .poll = poll,
    .save_state = save_state,
};
