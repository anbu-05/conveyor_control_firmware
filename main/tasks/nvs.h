#pragma once

/*
 * NVS task boundary.
 * Persistent settings and calibration storage are intentionally not implemented
 * yet; current NVS initialization only supports ESP-IDF services such as WiFi.
 */

#include "esp_err.h"

/* Initializes the default NVS partition for platform services. */
esp_err_t nvs_init(void);
