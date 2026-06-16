#include <stdio.h>

#include "app_state.h"
#include "config.h"
#include "conveyor_job.h"
#include "esp_err.h"
#include "esp_log.h"
#include "mqtt_task.h"
#include "runtime_config.h"
#include "sd_event_logger.h"

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

static void configure_sd_logging(void)
{
#if CONVEYOR_SD_LOG_ENABLED
    sdlog_config_t config = {
        .device_type = "conveyor",
        .device_id = CONVEYOR_ID,
        .project = "conveyor",
        .firmware_version = CONVEYOR_FIRMWARE_VERSION,
        .time_topic = CONVEYOR_MQTT_TOPIC_TIME,
        .config_text =
            "motor_count=1\n"
            "sensor_count=2\n"
            "actuator_M0_type=dc\n"
            "actuator_M0_pwm_gpio=7\n"
            "actuator_M0_dir_gpio=6\n"
            "actuator_M0_encoder_a_gpio=15\n"
            "actuator_M0_encoder_b_gpio=16\n"
            "input_S0_gpio=4\n"
            "input_S1_gpio=5\n"
            "mqtt_cmd_topic=" CONVEYOR_MQTT_TOPIC_CMD "\n"
            "mqtt_emergency_topic=" CONVEYOR_MQTT_TOPIC_EMERGENCY "\n"
            "mqtt_all_emergency_topic=" CONVEYOR_MQTT_TOPIC_ALL_EMERGENCY "\n"
            "mqtt_time_topic=" CONVEYOR_MQTT_TOPIC_TIME "\n",
        .sd_cs_gpio = CONVEYOR_SD_LOG_SD_CS_GPIO,
        .sd_mosi_gpio = CONVEYOR_SD_LOG_SD_MOSI_GPIO,
        .sd_sclk_gpio = CONVEYOR_SD_LOG_SD_SCLK_GPIO,
        .sd_miso_gpio = CONVEYOR_SD_LOG_SD_MISO_GPIO,
        .queue_length = CONVEYOR_SD_LOG_QUEUE_LENGTH,
        .stack_size = CONVEYOR_SD_LOG_TASK_STACK_SIZE,
        .task_priority = CONVEYOR_SD_LOG_TASK_PRIORITY,
        .flush_period_ms = CONVEYOR_SD_LOG_FLUSH_PERIOD_MS,
    };
    esp_err_t err = sdlog_start(&config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD logger disabled: %s", esp_err_to_name(err));
    }
#endif
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
    configure_sd_logging();
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
