#include <stdio.h>

#include "app_state.h"
#include "config.h"
#include "conveyor_job.h"
#include "esp_log.h"
#include "mqtt_task.h"
#include "runtime_config.h"

static const char *TAG = "conveyor";

/*
 * Configures standard input and output for command text.
 * ESP-IDF decides whether stdin/stdout use USB Serial/JTAG or UART based on
 * sdkconfig.
 */
static void configure_console(void)
{
    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
}

/*
 * ESP-IDF application entry point.
 * It creates shared state protection, configures hardware, and starts the
 * command, motor, and sensor tasks.
 */
void app_main(void)
{
    motor_mutex = xSemaphoreCreateMutex();
    if (motor_mutex == NULL) {
        ESP_LOGE(TAG, "failed to create motor mutex");
        return;
    }

    console_mutex = xSemaphoreCreateMutex();
    if (console_mutex == NULL) {
        ESP_LOGE(TAG, "failed to create console mutex");
        return;
    }

    configure_console();
    configure_runtime_config();
    configure_pwm();
    configure_sensors();
    configure_conveyor_job();

    xTaskCreate(conveyor_job_task, "conveyor_job", CONVEYOR_JOB_TASK_STACK_SIZE, NULL, CONVEYOR_JOB_TASK_PRIORITY, NULL);
    xTaskCreate(microrl_task, "microrl", MICRORL_TASK_STACK_SIZE, NULL, 5, NULL);
    xTaskCreate(motor_controller_task, "motor_ctrl_M0", MOTOR_TASK_STACK_SIZE, &motors[0], 5, NULL);
    xTaskCreate(sensor_reader_task, "sensor_reader", SENSOR_TASK_STACK_SIZE, NULL, 5, NULL);

#if CONVEYOR_MQTT_ENABLED
    configure_mqtt();
    xTaskCreate(mqtt_status_task, "mqtt_status", CONVEYOR_MQTT_STATUS_TASK_STACK_SIZE, NULL, CONVEYOR_MQTT_STATUS_TASK_PRIORITY, NULL);
#endif

    console_print("READY conveyor\r\n");
}
