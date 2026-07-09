#pragma once

// Runtime config API. Callers use enum keys; console string parsing lives in console.c.

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

esp_err_t runtime_config_get(runtime_config_key_t key, int32_t *out_value);
esp_err_t runtime_config_set(runtime_config_key_t key, int32_t value);
esp_err_t runtime_config_reset(runtime_config_key_t key);

esp_err_t runtime_config_load_nvs(runtime_config_key_t key);
esp_err_t runtime_config_store_nvs(runtime_config_key_t key);
