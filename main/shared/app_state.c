/*
 * Shared application state implementation.
 * This file intentionally keeps the motor, encoder, and sensor snapshot
 * together because they describe one mechanical axis. Tasks edit the exposed
 * axis struct directly while holding axis_mutex.
 */

#include "shared/app_state.h"

#include "config/config.h"

SemaphoreHandle_t axis_mutex;

axis_t axis = {
    .name = "axis",
    .pwm = 0,
    .direction = 0,
    .encoder_count = 0,
    .current_position = 0,
    .target_position = 0,
    .position_offset = 0,
    .target_speed = 0,
    .current_speed = 0,
    .position_control = false,
    .kp = 0.0f,
    .ki = 0.0f,
    .kd = 0.0f,
    .dir_gpio = (gpio_num_t)APP_AXIS_DIR_PIN,
    .pwm_gpio = (gpio_num_t)APP_AXIS_PWM_PIN,
    .encoder_a_gpio = (gpio_num_t)APP_AXIS_ENCODER_A_PIN,
    .encoder_b_gpio = (gpio_num_t)APP_AXIS_ENCODER_B_PIN,
    .positive_sensor_gpio = (gpio_num_t)APP_AXIS_POSITIVE_SENSOR_PIN,
    .negative_sensor_gpio = (gpio_num_t)APP_AXIS_NEGATIVE_SENSOR_PIN,
    .pwm_ledc_channel = LEDC_CHANNEL_0,
    .pcnt_unit = NULL,
    .positive_sensor = 0,
    .negative_sensor = 0,
};

/* Creates the mutex protecting the shared axis snapshot. */
esp_err_t app_state_init(void)
{
    axis_mutex = xSemaphoreCreateMutex();
    if (axis_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
