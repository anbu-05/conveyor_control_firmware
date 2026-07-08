#pragma once

/*
 * Learned axis-state persistence API.
 * This store owns calibrated values such as travel counts and reference flags;
 * editable runtime tuning stays in runtime_config.c.
 */

#include "esp_err.h"

/* Opens the axis-state NVS namespace before calibration values are read or saved. */
esp_err_t axis_store_init(void);
