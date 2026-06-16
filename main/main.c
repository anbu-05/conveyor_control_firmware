#include <stdio.h>

#include "app_state.h"
#include "config.h"
#include "conveyor_job.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "mqtt_task.h"
#include "runtime_config.h"

static const char *TAG = "conveyor";

/*
 * Configures standard input and output for command text.
 * The USB Serial/JTAG VFS routes microrl command input and console output to
 * the native USB port connected to the laptop.
 */
static void configure_console(void)
{
    usb_serial_jtag_driver_config_t config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();

    if (!usb_serial_jtag_is_driver_installed()) {
        ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&config));
    }

    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_CR);
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);
    usb_serial_jtag_vfs_use_driver();

    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);

    ESP_LOGI(TAG, "USB Serial/JTAG console ready");
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
    configure_encoders();
    configure_conveyor_job();

    xTaskCreate(conveyor_job_task, "conveyor_job", CONVEYOR_JOB_TASK_STACK_SIZE, NULL, CONVEYOR_JOB_TASK_PRIORITY, NULL);
    xTaskCreate(microrl_task, "microrl", MICRORL_TASK_STACK_SIZE, NULL, 5, NULL);
    xTaskCreate(motor_pid_task, "motor_pid_M0", MOTOR_PID_TASK_STACK_SIZE, &motors[0], 5, NULL);
    xTaskCreate(sensor_reader_task, "sensor_reader", SENSOR_TASK_STACK_SIZE, NULL, 5, NULL);

#if CONVEYOR_MQTT_ENABLED
    configure_mqtt();
    xTaskCreate(mqtt_status_task, "mqtt_status", CONVEYOR_MQTT_STATUS_TASK_STACK_SIZE, NULL, CONVEYOR_MQTT_STATUS_TASK_PRIORITY, NULL);
#endif

    console_print("READY conveyor\r\n");
}
