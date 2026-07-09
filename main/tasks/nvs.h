#pragma once

/*
 * Future NVS task boundary.
 * Persistent settings and calibration storage are intentionally not implemented
 * yet; current runtime_config values are flash defaults plus RAM updates only.
 */

#include "esp_err.h"

/* Placeholder init for the later NVS task. */
esp_err_t nvs_init(void);