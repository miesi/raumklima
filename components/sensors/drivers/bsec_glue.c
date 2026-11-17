#include "bsec_glue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "metrics.h"
#include "nvs.h"

static const char *TAG = "bsec";

#define NVS_NS        "bsec"
#define STATE_SAVE_US (4LL * 3600 * 1000 * 1000)   // BSEC learns over days, keep it across reboots

#if HUB_BSEC_VERSION == 3
extern const uint8_t bsec3_config_bme680[];
extern const uint8_t bsec3_config_bme690[];
#define CFG_LEN BSEC_MAX_PROPERTY_BLOB_SIZE        // Bosch config arrays have exactly this size
struct bsecg {
    void *inst;
};
#define CALL(fn, g, ...) fn((g)->inst, ##__VA_ARGS__)
#else
static const uint8_t k_bsec2_config_bme680[] = {
#include "bsec_iaq.txt"
};
struct bsecg {
    int unused;
};
static bool s_bsec2_used;                          // BSEC2 has one global state: one sensor only
#define CALL(fn, g, ...) fn(__VA_ARGS__)
#endif

int bsecg_version(void)
{
    return HUB_BSEC_VERSION;
}

static void state_load(bme_bsec_t *b)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t state[BSEC_MAX_STATE_BLOB_SIZE];
    size_t len = sizeof(state);
    if (nvs_get_blob(h, b->nvs_key, state, &len) == ESP_OK) {
        uint8_t *work = malloc(BSEC_MAX_WORKBUFFER_SIZE);
        if (work) {
            bsec_library_return_t r = CALL(bsec_set_state, b->bsec, state, len, work, BSEC_MAX_WORKBUFFER_SIZE);
            ESP_LOGI(TAG, "%s: state restored (%u bytes): %d", b->nvs_key, (unsigned)len, r);
            free(work);
        }
    }
    nvs_close(h);
}

void bme_bsec_save(bme_bsec_t *b)
{
    if (!b->bsec) {
        return;
    }
    uint8_t state[BSEC_MAX_STATE_BLOB_SIZE];
    uint32_t len = 0;
    uint8_t *work = malloc(BSEC_MAX_WORKBUFFER_SIZE);
    if (!work) {
        return;
    }
    bsec_library_return_t r = CALL(bsec_get_state, b->bsec, 0, state, sizeof(state), work, BSEC_MAX_WORKBUFFER_SIZE, &len);
    free(work);
    if (r != BSEC_OK) {
        ESP_LOGW(TAG, "bsec_get_state: %d", r);
        return;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, b->nvs_key, state, len);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "%s: state saved (%u bytes)", b->nvs_key, (unsigned)len);
    }
    b->last_save_us = esp_timer_get_time();
}

bool bme_bsec_init(bme_bsec_t *b, bsec_chip_t chip, bme_measure_fn measure, void *chip_ctx, const char **why)
{
    memset(b, 0, sizeof(*b));
    b->measure = measure;
    b->chip = chip_ctx;
    const uint8_t *cfg;
    uint32_t cfg_len;
#if HUB_BSEC_VERSION == 3
    cfg = chip == BSEC_CHIP_BME690 ? bsec3_config_bme690 : bsec3_config_bme680;
    cfg_len = CFG_LEN;
    bsecg_t *g = calloc(1, sizeof(bsecg_t));
    if (!g || !(g->inst = calloc(1, bsec_get_instance_size()))) {
        *why = "no memory";
        free(g);
        return false;
    }
#else
    if (chip != BSEC_CHIP_BME680) {
        *why = "needs BSEC3 (third_party/bsec3)";
        return false;
    }
    if (s_bsec2_used) {
        *why = "BSEC2 supports one sensor";
        return false;
    }
    cfg = k_bsec2_config_bme680;
    cfg_len = sizeof(k_bsec2_config_bme680);
    static bsecg_t g2;
    bsecg_t *g = &g2;
#endif
    if (CALL(bsec_init, g) != BSEC_OK) {
        *why = "bsec_init failed";
        return false;
    }
    bsec_version_t v;
    CALL(bsec_get_version, g, &v);
    snprintf(b->version, sizeof(b->version), "BSEC %d.%d.%d.%d", v.major, v.minor, v.major_bugfix, v.minor_bugfix);

    uint8_t *work = malloc(BSEC_MAX_WORKBUFFER_SIZE);
    if (!work) {
        *why = "no memory";
        return false;
    }
    bsec_library_return_t r = CALL(bsec_set_configuration, g, cfg, cfg_len, work, BSEC_MAX_WORKBUFFER_SIZE);
    free(work);
    if (r != BSEC_OK) {
        ESP_LOGE(TAG, "bsec_set_configuration: %d", r);
        *why = "bsec_set_configuration failed";
        return false;
    }
    b->bsec = g;
    // state blobs are not portable between BSEC versions: key carries chip and version
    snprintf(b->nvs_key, sizeof(b->nvs_key), "s%s_v%d", chip == BSEC_CHIP_BME690 ? "690" : "680", HUB_BSEC_VERSION);
    state_load(b);

    static const uint8_t outs[] = {
        BSEC_OUTPUT_IAQ,
        BSEC_OUTPUT_STATIC_IAQ,
        BSEC_OUTPUT_CO2_EQUIVALENT,
#if HUB_BSEC_VERSION == 3
        // BSEC3 IAQ: TVOC instead of breath-VOC (Bosch BSEC3 integration example, LP mode)
        BSEC_OUTPUT_TVOC_EQUIVALENT,
        BSEC_OUTPUT_GAS_PERCENTAGE,
#else
        BSEC_OUTPUT_BREATH_VOC_EQUIVALENT,
#endif
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
        req[i].sample_rate = BSEC_SAMPLE_RATE_LP;   // must match the 3 s config
    }
    bsec_sensor_configuration_t required[BSEC_MAX_PHYSICAL_SENSOR];
    uint8_t n_required = BSEC_MAX_PHYSICAL_SENSOR;
    r = CALL(bsec_update_subscription, g, req, sizeof(outs), required, &n_required);
    if (r < 0) {
        ESP_LOGE(TAG, "bsec_update_subscription: %d", r);
        *why = "bsec_update_subscription failed";
        b->bsec = NULL;
        return false;
    }
    if (r > 0) {
        ESP_LOGW(TAG, "bsec_update_subscription warning %d", r);
    }
#if HUB_BSEC_VERSION == 2
    s_bsec2_used = true;
#endif
    b->last_save_us = esp_timer_get_time();
    ESP_LOGI(TAG, "%s ready for %s, IAQ LP mode (3 s)", b->version, chip == BSEC_CHIP_BME690 ? "BME690" : "BME680");
    return true;
}

int64_t bme_bsec_poll(sensor_inst_t *s, bme_bsec_t *b, int64_t now_us)
{
    int64_t ts_ns = now_us * 1000;
    bsec_bme_settings_t st;
    bsec_library_return_t r = CALL(bsec_sensor_control, b->bsec, ts_ns, &st);
    if (r < 0) {
        ESP_LOGW(TAG, "bsec_sensor_control: %d", r);
        return -1;
    }
    if (st.trigger_measurement && st.op_mode == 1 /* forced */) {
        bme_sample_t d = {0};
        if (b->measure(b->chip, &st, &d) != ESP_OK) {
            return -1;
        }
        if (d.new_data) {
            bsec_input_t in[5];
            uint8_t ni = 0;
            const uint32_t pd = st.process_data;
            // heat source: self-heating of the board; 0 until calibrated against an SHT4x
            if (pd & (1 << (BSEC_INPUT_HEATSOURCE - 1))) {
                in[ni++] = (bsec_input_t){.sensor_id = BSEC_INPUT_HEATSOURCE, .signal = 0.0f, .time_stamp = ts_ns};
            }
            if (pd & BSEC_PROCESS_TEMPERATURE) {
                in[ni++] = (bsec_input_t){.sensor_id = BSEC_INPUT_TEMPERATURE, .signal = d.temperature, .time_stamp = ts_ns};
            }
            if (pd & BSEC_PROCESS_HUMIDITY) {
                in[ni++] = (bsec_input_t){.sensor_id = BSEC_INPUT_HUMIDITY, .signal = d.humidity, .time_stamp = ts_ns};
            }
            if (pd & BSEC_PROCESS_PRESSURE) {
                in[ni++] = (bsec_input_t){.sensor_id = BSEC_INPUT_PRESSURE, .signal = d.pressure, .time_stamp = ts_ns};
            }
            if ((pd & BSEC_PROCESS_GAS) && d.gas_valid) {
                in[ni++] = (bsec_input_t){.sensor_id = BSEC_INPUT_GASRESISTOR, .signal = d.gas_resistance, .time_stamp = ts_ns};
            }
            for (int i = 0; i < ni; i++) {
                in[i].signal_dimensions = 1;
            }
            bsec_output_t out[BSEC_NUMBER_OUTPUTS];
            uint8_t no = BSEC_NUMBER_OUTPUTS;
            r = CALL(bsec_do_steps, b->bsec, in, ni, out, &no);
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
                    b->iaq_accuracy = out[i].accuracy;
                    break;
                case BSEC_OUTPUT_STATIC_IAQ: sensor_publish(s, "static_iaq", v); break;
                case BSEC_OUTPUT_CO2_EQUIVALENT: sensor_publish(s, "co2eq_ppm", v); break;
#if HUB_BSEC_VERSION == 3
                case BSEC_OUTPUT_TVOC_EQUIVALENT: sensor_publish(s, "tvoc_ppb", v); break;
                case BSEC_OUTPUT_GAS_PERCENTAGE: sensor_publish(s, "gas_pct", v); break;
#else
                case BSEC_OUTPUT_BREATH_VOC_EQUIVALENT: sensor_publish(s, "bvoc_ppm", v); break;
#endif
                case BSEC_OUTPUT_RAW_TEMPERATURE: sensor_publish(s, "raw_temperature_c", v); break;
                case BSEC_OUTPUT_RAW_PRESSURE: sensor_publish(s, "pressure_hpa", v / 100.0f); break;
                case BSEC_OUTPUT_RAW_HUMIDITY: sensor_publish(s, "raw_humidity_pct", v); break;
                case BSEC_OUTPUT_RAW_GAS: sensor_publish(s, "gas_ohm", v); break;
                case BSEC_OUTPUT_STABILIZATION_STATUS: sensor_publish(s, "stab_status", v); break;
                case BSEC_OUTPUT_RUN_IN_STATUS: sensor_publish(s, "runin_status", v); break;
                case BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE: sensor_publish(s, "temperature_c", v); break;
                case BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY: sensor_publish(s, "humidity_pct", v); break;
                default: break;
                }
            }
        }
    }
    snprintf(s->info, sizeof(s->info), "%s, IAQ accuracy %u", b->version, b->iaq_accuracy);
    if (now_us - b->last_save_us > STATE_SAVE_US) {
        bme_bsec_save(b);
    }
    return st.next_call / 1000;
}

// --- detail page ---

static bool get(sensor_inst_t *s, const char *q, float *v)
{
    char key[METRIC_KEY_LEN];
    metric_t m;
    snprintf(key, sizeof(key), "%s.%s", s->name, q);
    if (!metrics_get(key, &m)) {
        return false;
    }
    *v = m.value;
    return true;
}

// Meaning of the IAQ accuracy, Bosch BSEC Integration Guide (BSEC 3.3, AN011, section 5.3.2.1, p. 51)
static const char *accuracy_text(int a)
{
    switch (a) {
    case 0: return "run-in, not reliable yet";
    case 1: return "low";
    case 2: return "medium, self-calibrating";
    case 3: return "high";
    default: return "?";
    }
}

void bme_bsec_detail(sensor_inst_t *s, const bme_bsec_t *b, char *buf, size_t len)
{
    float v, acc = -1;
    size_t o = 0;
#define ADD(...) o += snprintf(buf + o, o < len ? len - o : 0, __VA_ARGS__)
    get(s, "iaq_accuracy", &acc);
    if (get(s, "iaq", &v)) {
        ADD("IAQ          %.0f\n", v);
        ADD("Accuracy     %.0f of 3: %s\n", acc, accuracy_text((int)acc));
    }
    if (get(s, "static_iaq", &v)) {
        ADD("Static IAQ   %.0f\n", v);
    }
    if (get(s, "co2eq_ppm", &v)) {
        ADD("CO2 equiv.   %.0f ppm (estimate)\n", v);
    }
    if (get(s, "tvoc_ppb", &v)) {
        ADD("TVOC equiv.  %.0f ppb\n", v);
    }
    if (get(s, "bvoc_ppm", &v)) {
        ADD("bVOC equiv.  %.2f ppm\n", v);
    }
    if (get(s, "gas_ohm", &v)) {
        ADD("Gas          %.1f kOhm\n", v / 1000.0f);
    }
    float st = -1, ri = -1;
    get(s, "stab_status", &st);
    get(s, "runin_status", &ri);
    ADD("Stabilized   %s, run-in %s\n", st > 0.5f ? "yes" : "no", ri > 0.5f ? "done" : "ongoing");
    if (get(s, "temperature_c", &v)) {
        ADD("Temperature  %.1f \xc2\xb0" "C\n", v);
    }
    if (get(s, "humidity_pct", &v)) {
        ADD("Humidity     %.0f %%\n", v);
    }
    if (get(s, "pressure_hpa", &v)) {
        ADD("Pressure     %.0f hPa", v);
    }
    // Bosch BSEC Integration Guide (BSEC 3.3, AN011): IAQ p. 31, accuracy p. 10 and 51
    ADD("\fIAQ 0..500, lower is better\n"
        "50 = typical good air\n"
        "200 = typical polluted air\n"
        "relative: learns the last ~4 days\n"
        "Accuracy 0: run-in, IAQ not reliable\n"
        "1: let it see fresh air and bad air\n"
        "   (breath) ~30 min each\n"
        "2: self-calibration ongoing\n"
        "3: calibrated");
#undef ADD
}
