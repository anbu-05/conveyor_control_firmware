/*
 * ESP console task.
 * Later checkpoints register command handlers here; handlers translate user
 * input into typed state-machine events or read diagnostic snapshots.
 */

#include <stdio.h>
#include <stdbool.h>

#include "config/config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tasks/console_task.h"

/* Initializes console support; task creation stays in main.c. */
esp_err_t console_init(void)
{
    return ESP_OK;
}

/* Runs the console task and prints the boot-ready marker used during bring-up. */
void console_task(void *arg)
{
    (void)arg;
    printf("READY %s\n", APP_AXIS_APP_NAME);

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
