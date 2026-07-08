#pragma once

/*
 * Console task public API.
 * The console is an input producer and diagnostics surface; it calls public
 * motor APIs for local commissioning commands.
 */

#include "esp_err.h"

/* Initializes console support; main.c starts console_task() with xTaskCreate(). */
esp_err_t console_init(void);

/* Runs console handling after main.c creates this task. */
void console_task(void *arg);
