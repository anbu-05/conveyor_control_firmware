/*
 * Hardware service for the single axis.
 * This file initializes motor PWM, encoder PCNT, and physical sensor GPIOs,
 * then hardware_task() keeps the shared axis struct updated for other threads.
 */

#include "tasks/hardware_task.h"

#include <stdbool.h>
#include <stdint.h>
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

/* Sets up the direction GPIO and LEDC PWM channel for the motor driver. */
esp_err_t hardware_motor_init(void)
{
    gpio_config_t direction_gpio_config = {
        .pin_bit_mask = 1ULL << axis.dir_gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = HARDWARE_PWM_RESOLUTION,
        .freq_hz = APP_AXIS_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_channel_config_t pwm_channel = {
        .gpio_num = axis.pwm_gpio,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = axis.pwm_ledc_channel,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };

    ESP_RETURN_ON_ERROR(gpio_config(&direction_gpio_config), "hardware", "configure direction gpio");
    ESP_RETURN_ON_ERROR(gpio_set_level(axis.dir_gpio, APP_AXIS_NEGATIVE_DIR_LEVEL),
                        "hardware", "set default direction");
    ESP_RETURN_ON_ERROR(ledc_timer_config(&ledc_timer), "hardware", "configure ledc timer");
    ESP_RETURN_ON_ERROR(ledc_channel_config(&pwm_channel), "hardware", "configure pwm channel");
    stop_motor();
    return ESP_OK;
}

/* Sets up PCNT quadrature counting for the motor encoder GPIOs. */
esp_err_t hardware_encoder_init(void)
{
    pcnt_channel_handle_t channel_a = NULL;
    pcnt_channel_handle_t channel_b = NULL;
    pcnt_unit_config_t unit_config = {
        .low_limit = HARDWARE_PCNT_LOW_LIMIT,
        .high_limit = HARDWARE_PCNT_HIGH_LIMIT,
        .flags.accum_count = true,
    };
    pcnt_glitch_filter_config_t filter_config = {
        .max_glitch_ns = HARDWARE_PCNT_GLITCH_FILTER_NS,
    };
    pcnt_chan_config_t channel_a_config = {
        .edge_gpio_num = axis.encoder_a_gpio,
        .level_gpio_num = axis.encoder_b_gpio,
    };
    pcnt_chan_config_t channel_b_config = {
        .edge_gpio_num = axis.encoder_b_gpio,
        .level_gpio_num = axis.encoder_a_gpio,
    };
    gpio_config_t encoder_gpio_config = {
        .pin_bit_mask = (1ULL << axis.encoder_a_gpio) | (1ULL << axis.encoder_b_gpio),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_RETURN_ON_ERROR(gpio_config(&encoder_gpio_config), "hardware", "configure encoder gpio");
    ESP_RETURN_ON_ERROR(pcnt_new_unit(&unit_config, &axis.pcnt_unit), "hardware", "create pcnt unit");
    ESP_RETURN_ON_ERROR(pcnt_unit_set_glitch_filter(axis.pcnt_unit, &filter_config),
                        "hardware", "configure pcnt filter");
    ESP_RETURN_ON_ERROR(pcnt_new_channel(axis.pcnt_unit, &channel_a_config, &channel_a),
                        "hardware", "create pcnt channel a");
    ESP_RETURN_ON_ERROR(pcnt_new_channel(axis.pcnt_unit, &channel_b_config, &channel_b),
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

    ESP_RETURN_ON_ERROR(pcnt_unit_add_watch_point(axis.pcnt_unit, HARDWARE_PCNT_HIGH_LIMIT),
                        "hardware", "add pcnt high watch point");
    ESP_RETURN_ON_ERROR(pcnt_unit_add_watch_point(axis.pcnt_unit, HARDWARE_PCNT_LOW_LIMIT),
                        "hardware", "add pcnt low watch point");
    ESP_RETURN_ON_ERROR(pcnt_unit_enable(axis.pcnt_unit), "hardware", "enable pcnt");
    ESP_RETURN_ON_ERROR(pcnt_unit_clear_count(axis.pcnt_unit), "hardware", "clear pcnt count");
    ESP_RETURN_ON_ERROR(pcnt_unit_start(axis.pcnt_unit), "hardware", "start pcnt");
    return ESP_OK;
}

/* Sets up the positive and negative physical sensor GPIO inputs. */
esp_err_t hardware_sensor_init(void)
{
    gpio_config_t sensor_gpio_config = {
        .pin_bit_mask = (1ULL << axis.positive_sensor_gpio) |
                        (1ULL << axis.negative_sensor_gpio),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_RETURN_ON_ERROR(gpio_config(&sensor_gpio_config), "hardware", "configure sensor gpio");
    return ESP_OK;
}

/* Initializes all hardware owned by this service; main.c creates the task. */
esp_err_t hardware_init(void)
{
    ESP_RETURN_ON_ERROR(hardware_motor_init(), "hardware", "initialize motor");
    ESP_RETURN_ON_ERROR(hardware_encoder_init(), "hardware", "initialize encoder");
    ESP_RETURN_ON_ERROR(hardware_sensor_init(), "hardware", "initialize sensors");
    return ESP_OK;
}

/* Polls encoder and physical sensor state into axis forever. */
void hardware_task(void *arg)
{
    int encoder_count = 0;
    int positive_sensor = 0;
    int negative_sensor = 0;

    (void)arg;

    while (true) {
        positive_sensor = gpio_get_level(axis.positive_sensor_gpio);
        negative_sensor = gpio_get_level(axis.negative_sensor_gpio);

        if (axis_mutex != NULL) {
            xSemaphoreTake(axis_mutex, portMAX_DELAY);
        }

        if (axis.pcnt_unit != NULL && pcnt_unit_get_count(axis.pcnt_unit, &encoder_count) == ESP_OK) {
            axis.encoder_count = encoder_count;
            axis.current_position = encoder_count + axis.position_offset;
        }

        axis.positive_sensor = positive_sensor;
        axis.negative_sensor = negative_sensor;

        if (axis_mutex != NULL) {
            xSemaphoreGive(axis_mutex);
        }

        vTaskDelay(pdMS_TO_TICKS(APP_AXIS_CONTROL_PERIOD_MS));
    }
}

/* Sets motor direction and PWM; direction is a plain int matching config levels. */
esp_err_t set_motor(int direction, int pwm)
{
    int32_t configured_max = HARDWARE_PWM_MAX_DUTY;
    int duty = pwm;

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
    if (direction != APP_AXIS_NEGATIVE_DIR_LEVEL && direction != APP_AXIS_POSITIVE_DIR_LEVEL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, axis.pwm_ledc_channel, 0),
                        "hardware", "clear pwm before direction");
    ESP_RETURN_ON_ERROR(ledc_update_duty(LEDC_LOW_SPEED_MODE, axis.pwm_ledc_channel),
                        "hardware", "apply pwm clear");
    ESP_RETURN_ON_ERROR(gpio_set_level(axis.dir_gpio, direction), "hardware", "set motor direction");
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, axis.pwm_ledc_channel, duty),
                        "hardware", "set pwm duty");
    ESP_RETURN_ON_ERROR(ledc_update_duty(LEDC_LOW_SPEED_MODE, axis.pwm_ledc_channel),
                        "hardware", "apply pwm duty");

    if (axis_mutex != NULL) {
        xSemaphoreTake(axis_mutex, portMAX_DELAY);
    }

    axis.direction = direction;
    axis.pwm = duty;

    if (axis_mutex != NULL) {
        xSemaphoreGive(axis_mutex);
    }
    return ESP_OK;
}

/* Stops the motor by cutting PWM while preserving the last direction value. */
void stop_motor(void)
{
    (void)ledc_set_duty(LEDC_LOW_SPEED_MODE, axis.pwm_ledc_channel, 0);
    (void)ledc_update_duty(LEDC_LOW_SPEED_MODE, axis.pwm_ledc_channel);

    if (axis_mutex != NULL) {
        xSemaphoreTake(axis_mutex, portMAX_DELAY);
    }

    axis.pwm = 0;

    if (axis_mutex != NULL) {
        xSemaphoreGive(axis_mutex);
    }
}
