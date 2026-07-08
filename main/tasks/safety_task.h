#pragma once

/*
 * Safety monitor task public API.
 * Safety produces typed fault events for the state machine and may directly cut
 * PWM only for emergency stop or impossible unsafe conditions.
 */

#include "esp_err.h"

/* Initializes safety state; main.c starts safety_task() with xTaskCreate(). */
esp_err_t safety_init(void);

/* Runs safety monitoring after main.c creates this task. */
void safety_task(void *arg);
