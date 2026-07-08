/*
 * Hardware service for configured motors.
 * This file initializes motor PWM, encoder PCNT, and physical sensor GPIOs,
 * then hardware_task() keeps shared motor structs updated for other threads.
 */

#include "tasks/hardware.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "config/config.h"
#include "config/runtime_config.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/pulse_cnt.h"
#include "esp_check.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define HARDWARE_PCNT_GLITCH_FILTER_NS 1000
#define HARDWARE_PCNT_HIGH_LIMIT 32767
#define HARDWARE_PCNT_LOW_LIMIT -32768
#define HARDWARE_PWM_MAX_DUTY 255
#define HARDWARE_PWM_RESOLUTION LEDC_TIMER_8_BIT

/* Sets up the direction GPIO and LEDC PWM channel for one motor driver. */
esp_err_t hardware_motor_init(const char *motor_id)
{
    motor_t *motor = NULL;

    /* Resolve the requested string id against the configured motor array. */
    for (int i = 0; i < APP_MOTOR_COUNT; i++) {
        if (motor_id != NULL && strcmp(motors[i].id, motor_id) == 0) {
            motor = &motors[i];
            break;
        }
    }
    if (motor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Configure the direction pin as a plain output for this motor. */
    gpio_config_t direction_gpio_config = {
        .pin_bit_mask = 1ULL << motor->dir_gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    /* Configure one shared LEDC timer shape; each motor owns a channel. */
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = HARDWARE_PWM_RESOLUTION,
        .freq_hz = APP_AXIS_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };

    /* Route the selected LEDC channel to this motor's PWM pin. */
    ledc_channel_config_t pwm_channel = {
        .gpio_num = motor->pwm_gpio,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = motor->pwm_ledc_channel,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };

    ESP_RETURN_ON_ERROR(gpio_config(&direction_gpio_config), "hardware", "configure direction gpio");
    ESP_RETURN_ON_ERROR(gpio_set_level(motor->dir_gpio, APP_AXIS_NEGATIVE_DIR_LEVEL),
                        "hardware", "set default direction");
    ESP_RETURN_ON_ERROR(ledc_timer_config(&ledc_timer), "hardware", "configure ledc timer");
    ESP_RETURN_ON_ERROR(ledc_channel_config(&pwm_channel), "hardware", "configure pwm channel");
    stop_motor(motor_id);
    return ESP_OK;
}

/* Sets up PCNT quadrature counting for one motor encoder GPIO pair. */
esp_err_t hardware_encoder_init(const char *motor_id)
{
    motor_t *motor = NULL;
    pcnt_channel_handle_t channel_a = NULL;
    pcnt_channel_handle_t channel_b = NULL;

    /* Resolve the requested string id before touching hardware handles. */
    for (int i = 0; i < APP_MOTOR_COUNT; i++) {
        if (motor_id != NULL && strcmp(motors[i].id, motor_id) == 0) {
            motor = &motors[i];
            break;
        }
    }
    if (motor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Configure the PCNT unit limits and glitch filter for quadrature input. */
    pcnt_unit_config_t unit_config = {
        .low_limit = HARDWARE_PCNT_LOW_LIMIT,
        .high_limit = HARDWARE_PCNT_HIGH_LIMIT,
        .flags.accum_count = true,
    };
    pcnt_glitch_filter_config_t filter_config = {
        .max_glitch_ns = HARDWARE_PCNT_GLITCH_FILTER_NS,
    };

    /* Wire the two quadrature GPIOs as paired edge/level PCNT channels. */
    pcnt_chan_config_t channel_a_config = {
        .edge_gpio_num = motor->encoder_a_gpio,
        .level_gpio_num = motor->encoder_b_gpio,
    };
    pcnt_chan_config_t channel_b_config = {
        .edge_gpio_num = motor->encoder_b_gpio,
        .level_gpio_num = motor->encoder_a_gpio,
    };

    /* Configure both encoder pins as pulled-up inputs before PCNT owns them. */
    gpio_config_t encoder_gpio_config = {
        .pin_bit_mask = (1ULL << motor->encoder_a_gpio) | (1ULL << motor->encoder_b_gpio),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_RETURN_ON_ERROR(gpio_config(&encoder_gpio_config), "hardware", "configure encoder gpio");
    ESP_RETURN_ON_ERROR(pcnt_new_unit(&unit_config, &motor->pcnt_unit), "hardware", "create pcnt unit");
    ESP_RETURN_ON_ERROR(pcnt_unit_set_glitch_filter(motor->pcnt_unit, &filter_config),
                        "hardware", "configure pcnt filter");
    ESP_RETURN_ON_ERROR(pcnt_new_channel(motor->pcnt_unit, &channel_a_config, &channel_a),
                        "hardware", "create pcnt channel a");
    ESP_RETURN_ON_ERROR(pcnt_new_channel(motor->pcnt_unit, &channel_b_config, &channel_b),
                        "hardware", "create pcnt channel b");

    ESP_RETURN_ON_ERROR(pcnt_channel_set_edge_action(channel_a,
                                                     PCNT_CHANNEL_EDGE_ACTION_DECREASE,
                                                     PCNT_CHANNEL_EDGE_ACTION_INCREASE),
                        "hardware", "set pcnt channel a edge action");
    ESP_RETURN_ON_ERROR(pcnt_channel_set_level_action(channel_a,
                                                      PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                                      PCNT_CHANNEL_LEVEL_ACTION_INVERSE),
                        "hardware", "set pcnt channel a level action");
    ESP_RETURN_ON_ERROR(pcnt_channel_set_edge_action(channel_b,
                                                     PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                                     PCNT_CHANNEL_EDGE_ACTION_DECREASE),
                        "hardware", "set pcnt channel b edge action");
    ESP_RETURN_ON_ERROR(pcnt_channel_set_level_action(channel_b,
                                                      PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                                      PCNT_CHANNEL_LEVEL_ACTION_INVERSE),
                        "hardware", "set pcnt channel b level action");

    ESP_RETURN_ON_ERROR(pcnt_unit_add_watch_point(motor->pcnt_unit, HARDWARE_PCNT_HIGH_LIMIT),
                        "hardware", "add pcnt high watch point");
    ESP_RETURN_ON_ERROR(pcnt_unit_add_watch_point(motor->pcnt_unit, HARDWARE_PCNT_LOW_LIMIT),
                        "hardware", "add pcnt low watch point");
    ESP_RETURN_ON_ERROR(pcnt_unit_enable(motor->pcnt_unit), "hardware", "enable pcnt");
    ESP_RETURN_ON_ERROR(pcnt_unit_clear_count(motor->pcnt_unit), "hardware", "clear pcnt count");
    ESP_RETURN_ON_ERROR(pcnt_unit_start(motor->pcnt_unit), "hardware", "start pcnt");
    return ESP_OK;
}

/* Sets up one motor's positive and negative physical sensor GPIO inputs. */
esp_err_t hardware_sensor_init(const char *motor_id)
{
    motor_t *motor = NULL;

    /* Resolve the requested string id before configuring its GPIO pins. */
    for (int i = 0; i < APP_MOTOR_COUNT; i++) {
        if (motor_id != NULL && strcmp(motors[i].id, motor_id) == 0) {
            motor = &motors[i];
            break;
        }
    }
    if (motor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Configure the two physical sensor pins as direct GPIO inputs. */
    gpio_config_t sensor_gpio_config = {
        .pin_bit_mask = (1ULL << motor->positive_sensor_gpio) |
                        (1ULL << motor->negative_sensor_gpio),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_RETURN_ON_ERROR(gpio_config(&sensor_gpio_config), "hardware", "configure sensor gpio");
    return ESP_OK;
}

/* Initializes all hardware owned by this service for one motor id. */
esp_err_t hardware_init(const char *motor_id)
{
    /* Keep init order explicit: output first, then feedback inputs. */
    ESP_RETURN_ON_ERROR(hardware_motor_init(motor_id), "hardware", "initialize motor");
    ESP_RETURN_ON_ERROR(hardware_encoder_init(motor_id), "hardware", "initialize encoder");
    ESP_RETURN_ON_ERROR(hardware_sensor_init(motor_id), "hardware", "initialize sensors");
    return ESP_OK;
}

/* Polls encoder and physical sensor state into all configured motors forever. */
void hardware_task(void *arg)
{
    (void)arg;

    while (true) {
        for (int i = 0; i < APP_MOTOR_COUNT; i++) {
            int encoder_count = 0;

            /* Sample GPIO levels outside the mutex so hardware reads stay short. */
            const int positive_sensor = gpio_get_level(motors[i].positive_sensor_gpio);
            const int negative_sensor = gpio_get_level(motors[i].negative_sensor_gpio);

            if (motor_mutex != NULL) {
                xSemaphoreTake(motor_mutex, portMAX_DELAY);
            }

            /* Publish the latest PCNT count and offset-corrected position. */
            if (motors[i].pcnt_unit != NULL && pcnt_unit_get_count(motors[i].pcnt_unit, &encoder_count) == ESP_OK) {
                motors[i].encoder_count = encoder_count;
                motors[i].current_position = encoder_count + motors[i].position_offset;
            }

            /* Store raw sensor GPIO levels for diagnostics and safety logic. */
            motors[i].positive_sensor = positive_sensor;
            motors[i].negative_sensor = negative_sensor;

            if (motor_mutex != NULL) {
                xSemaphoreGive(motor_mutex);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(APP_AXIS_CONTROL_PERIOD_MS));
    }
}

/* Sets one motor direction and PWM; direction is a plain int matching config levels. */
esp_err_t set_motor(const char *motor_id, int pwm, int direction)
{
    motor_t *motor = NULL;
    int32_t configured_max = HARDWARE_PWM_MAX_DUTY;
    int duty = pwm;

    /* Resolve the requested motor id before validating command values. */
    for (int i = 0; i < APP_MOTOR_COUNT; i++) {
        if (motor_id != NULL && strcmp(motors[i].id, motor_id) == 0) {
            motor = &motors[i];
            break;
        }
    }
    if (motor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Clamp PWM against runtime max and hardware resolution. */
    (void)runtime_config_get(RUNTIME_CONFIG_MAX_PWM, &configured_max);
    if (configured_max < 0) {
        configured_max = 0;
    }
    if (configured_max > HARDWARE_PWM_MAX_DUTY) {
        configured_max = HARDWARE_PWM_MAX_DUTY;
    }
    if (duty < 0) {
        duty = 0;
    }
    if (duty > configured_max) {
        duty = (int)configured_max;
    }

    /* Reject directions outside the configured electrical levels. */
    if (direction != APP_AXIS_NEGATIVE_DIR_LEVEL && direction != APP_AXIS_POSITIVE_DIR_LEVEL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Force PWM to zero before direction changes to avoid hard reversals. */
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, motor->pwm_ledc_channel, 0),
                        "hardware", "clear pwm before direction");
    ESP_RETURN_ON_ERROR(ledc_update_duty(LEDC_LOW_SPEED_MODE, motor->pwm_ledc_channel),
                        "hardware", "apply pwm clear");
    ESP_RETURN_ON_ERROR(gpio_set_level(motor->dir_gpio, direction), "hardware", "set motor direction");
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, motor->pwm_ledc_channel, duty),
                        "hardware", "set pwm duty");
    ESP_RETURN_ON_ERROR(ledc_update_duty(LEDC_LOW_SPEED_MODE, motor->pwm_ledc_channel),
                        "hardware", "apply pwm duty");

    if (motor_mutex != NULL) {
        xSemaphoreTake(motor_mutex, portMAX_DELAY);
    }

    /* Publish the output that was actually applied after clamping. */
    motor->direction = direction;
    motor->pwm = duty;

    if (motor_mutex != NULL) {
        xSemaphoreGive(motor_mutex);
    }
    return ESP_OK;
}

/* Stops one motor by cutting PWM while preserving the last direction value. */
void stop_motor(const char *motor_id)
{
    motor_t *motor = NULL;

    /* Resolve the requested motor id; invalid stop requests are ignored safely. */
    for (int i = 0; i < APP_MOTOR_COUNT; i++) {
        if (motor_id != NULL && strcmp(motors[i].id, motor_id) == 0) {
            motor = &motors[i];
            break;
        }
    }
    if (motor == NULL) {
        return;
    }

    /* Cut the hardware duty immediately before updating shared state. */
    (void)ledc_set_duty(LEDC_LOW_SPEED_MODE, motor->pwm_ledc_channel, 0);
    (void)ledc_update_duty(LEDC_LOW_SPEED_MODE, motor->pwm_ledc_channel);

    if (motor_mutex != NULL) {
        xSemaphoreTake(motor_mutex, portMAX_DELAY);
    }

    motor->pwm = 0;

    if (motor_mutex != NULL) {
        xSemaphoreGive(motor_mutex);
    }
}
