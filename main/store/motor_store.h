#pragma once

/*
 * Learned motor-state persistence API.
 * This store owns calibrated values such as travel counts and reference flags;
 * editable runtime tuning stays in runtime_config.c.
 */

#include "esp_err.h"

/* Opens the motor-state NVS namespace before calibration values are read or saved. */
esp_err_t motor_store_init(void);
