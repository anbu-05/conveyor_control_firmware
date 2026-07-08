/*
 * Safety monitor task placeholder.
 * Active safety can qualify sensors, direction checks, stalls, and emergency
 * stops after base local movement and state ownership are established.
 */

#include "tasks/safety.h"

#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Initializes future safety state; task creation stays in main.c. */
esp_err_t safety_init(void)
{
    return ESP_OK;
}

/* Runs the future safety monitor; currently idle until motion exists. */
void safety_task(void *arg)
{
    (void)arg;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
