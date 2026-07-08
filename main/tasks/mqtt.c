/*
 * MQTT command/result/status task placeholder.
 * App-specific topics and commands belong here after local motion, safety, and
 * persistence restore are working through console control.
 */

#include "tasks/mqtt.h"

#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Initializes future MQTT state; task creation stays in main.c. */
esp_err_t mqtt_init(void)
{
    return ESP_OK;
}

/* Runs the future MQTT loop; currently idle until networking is added. */
void mqtt_task(void *arg)
{
    (void)arg;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
