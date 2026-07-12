/*
 * Firmware entrypoint.
 * main.c is intentionally limited to platform/service initialization and task
 * startup. All xTaskCreate calls stay here so boot order and thread ownership
 * are visible in one file.
 */

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config/config.h"
#include "shared/app_state.h"
#include "statemachine/statemachine.h"
#include "tasks/console.h"
#include "tasks/hardware.h"
#include "tasks/nvs.h"
#include "tasks/pid.h"
#include "tasks/mqtt.h"
#include "tasks/safety.h"

static const char *TAG = APP_MOTOR_APP_NAME;

#define HARDWARE_TASK_STACK_SIZE 3072
#define MOTOR_PID_TASK_STACK_SIZE 4096
#define SAFETY_TASK_STACK_SIZE 3072
#define STATEMACHINE_TASK_STACK_SIZE 4096
#define MQTT_TASK_STACK_SIZE 4096
#define CONSOLE_TASK_STACK_SIZE 4096

#define HARDWARE_TASK_PRIORITY 5
#define MOTOR_PID_TASK_PRIORITY 5
#define SAFETY_TASK_PRIORITY 6
#define STATEMACHINE_TASK_PRIORITY 6
#define MQTT_TASK_PRIORITY 4
#define CONSOLE_TASK_PRIORITY 4

/* Logs startup failures without stopping the rest of the boot sequence. */
static void log_if_error(const char *name, esp_err_t err)
{
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s failed: %s", name, esp_err_to_name(err));
    }
}

/* ESP-IDF entrypoint that initializes storage, config, hardware, and tasks. */
void app_main(void)
{
    log_if_error("nvs_init", nvs_init());
    log_if_error("app_state_init", app_state_init());
    for (int i = 0; i < APP_MOTOR_COUNT; i++) {
        const char *motor_id = NULL;

        log_if_error("hardware_get_motor_id", hardware_get_motor_id(i, &motor_id));
        if (motor_id == NULL) {
            continue;
        }
        log_if_error("hardware_init", hardware_init(motor_id));
        /* Start in position mode so existing boot behavior stays unchanged until speed mode is selected intentionally. */
        log_if_error("motor_pid_init", motor_pid_init(motor_id, MOTOR_PID_MODE_POSITION));
    }
    log_if_error("safety_init", safety_init());
    log_if_error("statemachine_init", statemachine_init());
    log_if_error("mqtt_init", mqtt_init());
    log_if_error("console_init", console_init());

    xTaskCreate(hardware_task, "hardware_task", HARDWARE_TASK_STACK_SIZE, NULL, HARDWARE_TASK_PRIORITY, NULL);
    for (int i = 0; i < APP_MOTOR_COUNT; i++) {
        const char *motor_id = NULL;

        if (hardware_get_motor_id(i, &motor_id) != ESP_OK) {
            continue;
        }
        xTaskCreate(motor_pid_task, "motor_pid_task", MOTOR_PID_TASK_STACK_SIZE,
                    (void *)motor_id, MOTOR_PID_TASK_PRIORITY, NULL);
    }
    xTaskCreate(safety_task, "safety_task", SAFETY_TASK_STACK_SIZE, NULL, SAFETY_TASK_PRIORITY, NULL);
    xTaskCreate(statemachine_task, "statemachine_task", STATEMACHINE_TASK_STACK_SIZE, NULL, STATEMACHINE_TASK_PRIORITY, NULL);
    xTaskCreate(mqtt_task, "mqtt_task", MQTT_TASK_STACK_SIZE, NULL, MQTT_TASK_PRIORITY, NULL);
    xTaskCreate(console_task, "console_task", CONSOLE_TASK_STACK_SIZE, NULL, CONSOLE_TASK_PRIORITY, NULL);
}
