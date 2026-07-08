#pragma once

/*
 * Console task public API.
 * The console is an input producer and diagnostics surface; it must submit axis
 * requests to the state-machine queue instead of changing app state directly.
 */

#include "esp_err.h"

/* Initializes console support; main.c starts console_task() with xTaskCreate(). */
esp_err_t console_init(void);

/* Runs console handling after main.c creates this task. */
void console_task(void *arg);
