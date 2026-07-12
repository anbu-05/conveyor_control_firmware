/*
 * Shared application state implementation.
 * This file intentionally keeps the motor, encoder, and sensor snapshot
 * together because they describe one mechanical motor. app_state is storage;
 * owner modules expose behavior APIs for the fields they own.
 */

#include "shared/app_state.h"

#include "config/config.h"

SemaphoreHandle_t motor_mutex;

motor_t motors[APP_MOTOR_COUNT] = {
    {
        .id = "M0",
        .pwm = 0,
        .direction = 0,
        .encoder_count = 0,
        .current_position = 0,
        .target_position = 0,
        .speed = 0,
        .target_speed = 0,
        .position_offset = 0,
        .PID_control = true,
        .pid_mode = MOTOR_PID_MODE_POSITION,
        .kp = 0.5f,
        .ki = 0.0f,
        .kd = 0.05f,
        .integral = 0.0f,
        .previous_error = 0.0f,
        .has_previous_error = false,
        .rpwm_gpio = MOTOR_RPWM_GPIO,
        .lpwm_gpio = MOTOR_LPWM_GPIO,
        .ren_gpio = MOTOR_REN_GPIO,
        .len_gpio = MOTOR_LEN_GPIO,
        .encoder_a_gpio = (gpio_num_t)APP_MOTOR_ENCODER_A_PIN,
        .encoder_b_gpio = (gpio_num_t)APP_MOTOR_ENCODER_B_PIN,
        .upstream_sensor_gpio = (gpio_num_t)APP_MOTOR_UPSTREAM_SENSOR_PIN,
        .downstream_sensor_gpio = (gpio_num_t)APP_MOTOR_DOWNSTREAM_SENSOR_PIN,
        .rpwm_ledc_channel = LEDC_CHANNEL_0,
        .lpwm_ledc_channel = LEDC_CHANNEL_1,
        .pcnt_unit = NULL,
        .upstream_sensor = 0,
        .downstream_sensor = 0,
    },
};

/* Creates the mutex protecting shared motor snapshots. */
esp_err_t app_state_init(void)
{
    motor_mutex = xSemaphoreCreateMutex();
    if (motor_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
