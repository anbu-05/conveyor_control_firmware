#pragma once

// Runtime config API. Values live in RAM and can be loaded from defaults or NVS.

#include <stdint.h>

#include "esp_err.h"

typedef enum {
    RUNTIME_CONFIG_PID_KP_MILLI,
    RUNTIME_CONFIG_PID_KI_MILLI,
    RUNTIME_CONFIG_PID_KD_MILLI,
    RUNTIME_CONFIG_MAX_PWM,
    RUNTIME_CONFIG_MIN_START_PWM,
    RUNTIME_CONFIG_REFERENCE_SPEED_COUNTS_PER_SEC,
    RUNTIME_CONFIG_POSITIVE_SPEED_COUNTS_PER_SEC,
    RUNTIME_CONFIG_NEGATIVE_SPEED_COUNTS_PER_SEC,
    RUNTIME_CONFIG_SENSOR_SEEK_SPEED_COUNTS_PER_SEC,
    RUNTIME_CONFIG_MAX_SPEED_COUNTS_PER_SEC,
    RUNTIME_CONFIG_POSITION_TOLERANCE_COUNTS,
    RUNTIME_CONFIG_REFERENCE_TIMEOUT_MS,
    RUNTIME_CONFIG_POSITIVE_TIMEOUT_MS,
    RUNTIME_CONFIG_NEGATIVE_TIMEOUT_MS,
    RUNTIME_CONFIG_STALL_CHECK_MS,
    RUNTIME_CONFIG_STALL_MIN_DELTA_COUNTS,
    RUNTIME_CONFIG_DIRECTION_CHECK_DELAY_MS,
    RUNTIME_CONFIG_LIMIT_SWITCH_QUALIFY_MS,
    RUNTIME_CONFIG_MQTT_STATUS_PERIOD_MS,
    RUNTIME_CONFIG_COUNT,
} runtime_config_key_t;

typedef struct {
    int32_t pid_kp_milli;
    int32_t pid_ki_milli;
    int32_t pid_kd_milli;
    int32_t max_pwm;
    int32_t min_start_pwm;
    int32_t reference_speed_counts_per_sec;
    int32_t positive_speed_counts_per_sec;
    int32_t negative_speed_counts_per_sec;
    int32_t sensor_seek_speed_counts_per_sec;
    int32_t max_speed_counts_per_sec;
    int32_t position_tolerance_counts;
    int32_t reference_timeout_ms;
    int32_t positive_timeout_ms;
    int32_t negative_timeout_ms;
    int32_t stall_check_ms;
    int32_t stall_min_delta_counts;
    int32_t direction_check_delay_ms;
    int32_t limit_switch_qualify_ms;
    int32_t mqtt_status_period_ms;
} runtime_config_t;

esp_err_t runtime_config_get(runtime_config_key_t key, int32_t *out_value);
esp_err_t runtime_config_set(runtime_config_key_t key, int32_t value);

esp_err_t runtime_config_load_default(runtime_config_key_t key);
esp_err_t runtime_config_load_nvs(runtime_config_key_t key);
esp_err_t runtime_config_store_nvs(runtime_config_key_t key);

void runtime_config_load_defaults(void);
esp_err_t runtime_config_load_all_nvs(void);
esp_err_t runtime_config_store_all_nvs(void);
