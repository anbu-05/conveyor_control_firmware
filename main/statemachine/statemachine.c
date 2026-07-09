/*
 * Motor state-machine implementation.
 * This module owns command acceptance, app states, calibration flow, and
 * motion completion/failure decisions. The skeleton only wires the ownership
 * boundary; the event queue and transitions are added in later checkpoints.
 */

#include "statemachine/statemachine.h"

#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Initializes future state-machine state; task creation stays in main.c. */
esp_err_t statemachine_init(void)
{
    return ESP_OK;
}

/* FreeRTOS task entrypoint that will consume motor events in a later checkpoint. */
void statemachine_task(void *arg)
{
    (void)arg;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* Queues a motor event for the state machine; currently returns false until the queue exists. */
bool statemachine_send_event(const motor_event_t *event)
{
    (void)event;
    return false;
}
