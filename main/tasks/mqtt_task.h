#pragma once

/*
 * MQTT task public API.
 * MQTT is deferred until local motion works; when enabled it will parse backend
 * payloads into the same typed events used by console commands.
 */

#include "esp_err.h"

/* Initializes MQTT state; main.c starts mqtt_task() with xTaskCreate(). */
esp_err_t mqtt_init(void);

/* Runs MQTT handling after main.c creates this task. */
void mqtt_task(void *arg);
