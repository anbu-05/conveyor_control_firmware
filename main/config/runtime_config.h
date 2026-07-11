#pragma once

// Runtime config API. Callers use enum keys; console string parsing lives in console.c.

#include <stdint.h>

#include "esp_err.h"

typedef enum {
    /* PID gains are intentionally absent; they belong to motor_t so each motor can tune independently. */
    RUNTIME_CONFIG_MAX_PWM,                   // Maximum PWM duty allowed for raw and PID motor output.
    RUNTIME_CONFIG_POSITION_TOLERANCE_COUNTS, // Position error deadband in encoder counts.
    RUNTIME_CONFIG_COUNT,
} runtime_config_key_t;

esp_err_t runtime_config_get(runtime_config_key_t key, int32_t *out_value);
esp_err_t runtime_config_set(runtime_config_key_t key, int32_t value);
esp_err_t runtime_config_reset(runtime_config_key_t key);
