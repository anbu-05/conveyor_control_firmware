#pragma once

/*
 * State-machine public boundary.
 * Console, safety, and MQTT submit tray jobs here; they must not call internal
 * transition code because statemachine_task() serializes conveyor behavior.
 * statemachine_jobrx() and statemachine_jobtx() block until completion; do not
 * call them from statemachine_task().
 */

#include "esp_err.h"

typedef enum {
    STATEMACHINE_RESULT_RX_DONE,
    STATEMACHINE_RESULT_TX_DONE,
    STATEMACHINE_RESULT_TRAY_ALREADY_PRESENT,
    STATEMACHINE_RESULT_TRAY_NOT_RECEIVED,
    STATEMACHINE_RESULT_TRAY_TRANSFER_STUCK,
    STATEMACHINE_RESULT_NO_TRAY_PRESENT,
    STATEMACHINE_RESULT_TRAY_HANDOFF_STUCK,
    STATEMACHINE_RESULT_EMERGENCY_STOP,
    STATEMACHINE_RESULT_JOB_TIMEOUT,
    STATEMACHINE_RESULT_JOB_REJECTED,
} statemachine_result_t;

typedef enum {
    STATEMACHINE_STATUS_IDLE,
    STATEMACHINE_STATUS_RECEIVE_WAITING_FOR_TRAY,
    STATEMACHINE_STATUS_RECEIVE_MOVING_TRAY,
    STATEMACHINE_STATUS_RECEIVE_TRAY_RECEIVED,
    STATEMACHINE_STATUS_TRANSMIT_TRANSMITTING_TRAY,
    STATEMACHINE_STATUS_TRANSMIT_TRAY_HANDED_OFF,
} statemachine_status_t;

/* Initializes state-machine state; main.c starts statemachine_task() with xTaskCreate(). */
esp_err_t statemachine_init(void);

/*
 * FreeRTOS task entrypoint that owns state transitions.
 * Named explicitly so task ownership is visible at call sites and diagnostics.
 */
void statemachine_task(void *arg);

/* Queues one receive job and blocks until statemachine_task() returns its result. */
statemachine_result_t statemachine_jobrx(void);

/* Queues one transmit job and blocks until statemachine_task() returns its result. */
statemachine_result_t statemachine_jobtx(void);

/* Returns the current state-machine status for polling live progress. */
statemachine_status_t statemachine_get_status(void);
