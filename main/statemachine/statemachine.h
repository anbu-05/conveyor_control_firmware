#pragma once

/*
 * State-machine public boundary.
 * Console, safety, and MQTT will submit typed events here; they must not call
 * transition functions directly because statemachine_task() serializes all
 * high-level axis state changes.
 */

#include <stdbool.h>

#include "esp_err.h"

/* Opaque until checkpoint 4 defines the typed command/fault event payload. */
typedef struct axis_event axis_event_t;

/* Initializes state-machine state; main.c starts statemachine_task() with xTaskCreate(). */
esp_err_t statemachine_init(void);

/*
 * FreeRTOS task entrypoint that owns state transitions.
 * Named explicitly so task ownership is visible at call sites and diagnostics.
 */
void statemachine_task(void *arg);

/*
 * Named statemachine_send_event() because producers enqueue requests; they do
 * not directly invoke reference/positive/negative transitions owned by
 * statemachine_task().
 */
bool statemachine_send_event(const axis_event_t *event);
