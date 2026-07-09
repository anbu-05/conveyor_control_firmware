#pragma once

// Runtime config API. Callers use enum keys; console string parsing lives in console.c.

#include <stdint.h>

#include "esp_err.h"

typedef enum {
    RUNTIME_CONFIG_PID_KP_MILLI,              // Position PID proportional gain in milli-units.
    RUNTIME_CONFIG_PID_KI_MILLI,              // Position PID integral gain in milli-units.
    RUNTIME_CONFIG_PID_KD_MILLI,              // Position PID derivative gain in milli-units.
    RUNTIME_CONFIG_MAX_PWM,                   // Maximum PWM duty allowed for raw and PID motor output.
    RUNTIME_CONFIG_MAX_SPEED_COUNTS_PER_SEC,  // Signed PID speed command clamp in encoder counts/sec.
    RUNTIME_CONFIG_POSITION_TOLERANCE_COUNTS, // Position error window where PID stops the motor.
    RUNTIME_CONFIG_COUNT,
} runtime_config_key_t;

esp_err_t runtime_config_get(runtime_config_key_t key, int32_t *out_value);
esp_err_t runtime_config_set(runtime_config_key_t key, int32_t value);
esp_err_t runtime_config_reset(runtime_config_key_t key);
