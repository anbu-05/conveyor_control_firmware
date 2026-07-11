#pragma once

/*
 * MQTT task public API.
 * Backend networking stays behind this small boundary; command parsing,
 * publishing, and private status polling remain internal to tasks/mqtt.c.
 */

#include "esp_err.h"

/* Initializes WiFi/MQTT state; main.c starts mqtt_task() with xTaskCreate(). */
esp_err_t mqtt_init(void);

/* Runs blocking MQTT command handling after main.c creates this task. */
void mqtt_task(void *arg);
