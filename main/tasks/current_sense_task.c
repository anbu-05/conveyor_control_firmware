#include "app_state.h"

#include <stdint.h>

#include "config.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "runtime_config.h"

#define ADC_FALLBACK_FULL_SCALE_MV 3300
#define ADC_FALLBACK_RAW_MAX 4095
#define CURRENT_AVG_SAMPLE_COUNT 10

typedef struct {
    adc_oneshot_unit_handle_t unit;
    adc_cali_handle_t lis_cali;
    adc_cali_handle_t ris_cali;
    bool lis_cali_enabled;
    bool ris_cali_enabled;
} current_adc_t;

static bool create_cali_handle(adc_channel_t channel, adc_cali_handle_t *handle)
{
    esp_err_t err = ESP_FAIL;

    if (handle == NULL) {
        return false;
    }

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .chan = channel,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };

    err = adc_cali_create_scheme_curve_fitting(&cali_config, handle);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };

    err = adc_cali_create_scheme_line_fitting(&cali_config, handle);
#endif

    return err == ESP_OK;
}

static void configure_adc(current_adc_t *adc, const motor_t *motor)
{
    adc_oneshot_unit_init_cfg_t unit_config = {
        .unit_id = ADC_UNIT_1,
    };
    adc_oneshot_chan_cfg_t channel_config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };

    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_config, &adc->unit));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc->unit, motor->lis_adc_channel, &channel_config));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc->unit, motor->ris_adc_channel, &channel_config));

    adc->lis_cali_enabled = create_cali_handle(motor->lis_adc_channel, &adc->lis_cali);
    adc->ris_cali_enabled = create_cali_handle(motor->ris_adc_channel, &adc->ris_cali);
}

static int raw_to_mv(int raw, adc_cali_handle_t cali_handle, bool cali_enabled)
{
    int voltage = 0;

    if (cali_enabled && adc_cali_raw_to_voltage(cali_handle, raw, &voltage) == ESP_OK) {
        return voltage;
    }

    return (raw * ADC_FALLBACK_FULL_SCALE_MV) / ADC_FALLBACK_RAW_MAX;
}

static bool read_channel_mv(current_adc_t *adc, adc_channel_t channel, adc_cali_handle_t cali_handle,
                            bool cali_enabled, int *mv)
{
    int raw = 0;
    int64_t sum_mv = 0;

    if (mv == NULL) {
        return false;
    }

    for (int i = 0; i < BTS7960_CURRENT_SAMPLE_COUNT; i++) {
        if (adc_oneshot_read(adc->unit, channel, &raw) != ESP_OK) {
            return false;
        }

        sum_mv += raw_to_mv(raw, cali_handle, cali_enabled);
    }

    *mv = (int)(sum_mv / BTS7960_CURRENT_SAMPLE_COUNT);
    return true;
}

static int adc_mv_to_bts_mv(int adc_mv)
{
    int64_t numerator = (int64_t)adc_mv *
                        (BTS7960_CURRENT_DIVIDER_TOP_OHMS + BTS7960_CURRENT_DIVIDER_BOTTOM_OHMS);

    return (int)(numerator / BTS7960_CURRENT_DIVIDER_BOTTOM_OHMS);
}

static int bts_mv_to_current_mA(int bts_mv)
{
    int64_t numerator = (int64_t)bts_mv * runtime_config_k_ilis();

    return (int)(numerator / BTS7960_CURRENT_R_IS_OHMS);
}

static int max_int(int a, int b)
{
    if (a > b) {
        return a;
    }

    return b;
}

void current_sense_task(void *arg)
{
    motor_t *motor = (motor_t *)arg;
    current_adc_t adc = {0};
    int lis_adc_mv = 0;
    int ris_adc_mv = 0;
    int lis_bts_mv = 0;
    int ris_bts_mv = 0;
    int lis_current_mA = 0;
    int ris_current_mA = 0;
    int current_mA = 0;
    int current_avg_mA = 0;
    int current_samples[CURRENT_AVG_SAMPLE_COUNT] = {0};
    int current_sample_index = 0;
    int current_sample_count = 0;
    int current_sample_sum = 0;
    bool sample_ok = false;

    configure_adc(&adc, motor);

    while (1) {
        sample_ok = read_channel_mv(&adc, motor->lis_adc_channel, adc.lis_cali, adc.lis_cali_enabled, &lis_adc_mv) &&
                    read_channel_mv(&adc, motor->ris_adc_channel, adc.ris_cali, adc.ris_cali_enabled, &ris_adc_mv);

        if (sample_ok) {
            lis_bts_mv = adc_mv_to_bts_mv(lis_adc_mv);
            ris_bts_mv = adc_mv_to_bts_mv(ris_adc_mv);
            lis_current_mA = bts_mv_to_current_mA(lis_bts_mv);
            ris_current_mA = bts_mv_to_current_mA(ris_bts_mv);

            current_mA = max_int(ris_current_mA, lis_current_mA);

            current_sample_sum -= current_samples[current_sample_index];
            current_samples[current_sample_index] = current_mA;
            current_sample_sum += current_mA;
            if (current_sample_count < CURRENT_AVG_SAMPLE_COUNT) {
                current_sample_count++;
            }
            current_sample_index++;
            if (current_sample_index >= CURRENT_AVG_SAMPLE_COUNT) {
                current_sample_index = 0;
            }
            current_avg_mA = current_sample_sum / current_sample_count;
        }

        xSemaphoreTake(motor_mutex, portMAX_DELAY);
        if (sample_ok) {
            motor->lis_adc_mv = lis_adc_mv;
            motor->ris_adc_mv = ris_adc_mv;
            motor->lis_bts_mv = lis_bts_mv;
            motor->ris_bts_mv = ris_bts_mv;
            motor->lis_current_mA = lis_current_mA;
            motor->ris_current_mA = ris_current_mA;
            motor->current_mA = current_mA;
            motor->current_avg_mA = current_avg_mA;
        }
        motor->current_sample_ok = sample_ok;
        xSemaphoreGive(motor_mutex);

        vTaskDelay(pdMS_TO_TICKS(BTS7960_CURRENT_SAMPLE_DELAY_MS));
    }
}
