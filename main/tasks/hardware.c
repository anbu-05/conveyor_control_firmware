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

static bool s_direction_flipped;

/* Sets up BTS7960 enable GPIOs and paired LEDC PWM channels for one motor driver. */
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

    /* Configure both BTS7960 enable pins as plain outputs for this motor. */
    gpio_config_t enable_gpio_config = {
        .pin_bit_mask = (1ULL << motor->ren_gpio) | (1ULL << motor->len_gpio),
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
        .freq_hz = APP_MOTOR_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };

    /* Route one LEDC channel to each BTS7960 PWM input. */
    ledc_channel_config_t rpwm_channel = {
        .gpio_num = motor->rpwm_gpio,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = motor->rpwm_ledc_channel,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    ledc_channel_config_t lpwm_channel = {
        .gpio_num = motor->lpwm_gpio,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = motor->lpwm_ledc_channel,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };

    ESP_RETURN_ON_ERROR(gpio_config(&enable_gpio_config), "hardware", "configure bts7960 enable gpio");
    ESP_RETURN_ON_ERROR(gpio_set_level(motor->ren_gpio, 1), "hardware", "enable bts7960 right side");
    ESP_RETURN_ON_ERROR(gpio_set_level(motor->len_gpio, 1), "hardware", "enable bts7960 left side");
    ESP_RETURN_ON_ERROR(ledc_timer_config(&ledc_timer), "hardware", "configure ledc timer");
    ESP_RETURN_ON_ERROR(ledc_channel_config(&rpwm_channel), "hardware", "configure rpwm channel");
    ESP_RETURN_ON_ERROR(ledc_channel_config(&lpwm_channel), "hardware", "configure lpwm channel");
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

/* Sets up one motor's upstream and downstream physical sensor GPIO inputs. */
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
        .pin_bit_mask = (1ULL << motor->upstream_sensor_gpio) |
                        (1ULL << motor->downstream_sensor_gpio),
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
    /* Keep speed derivation local to the hardware publisher so motor_t only stores public snapshots. */
    static int previous_position[APP_MOTOR_COUNT] = {0};
    static bool has_previous_position[APP_MOTOR_COUNT] = {false};

    while (true) {
        for (int i = 0; i < APP_MOTOR_COUNT; i++) {
            int encoder_count = 0;

            /* Sample GPIO levels outside the mutex so hardware reads stay short. */
            const int upstream_sensor = gpio_get_level(motors[i].upstream_sensor_gpio);
            const int downstream_sensor = gpio_get_level(motors[i].downstream_sensor_gpio);

            if (motor_mutex != NULL) {
                xSemaphoreTake(motor_mutex, portMAX_DELAY);
            }

            /* Publish speed from the same offset-corrected position path so PID and diagnostics share one basis. */
            if (motors[i].pcnt_unit != NULL && pcnt_unit_get_count(motors[i].pcnt_unit, &encoder_count) == ESP_OK) {
                const int current_position = encoder_count + motors[i].position_offset;

                motors[i].encoder_count = encoder_count;
                motors[i].current_position = current_position;
                motors[i].speed = has_previous_position[i] ?
                                  ((current_position - previous_position[i]) * 1000) / APP_MOTOR_CONTROL_PERIOD_MS : 0;
                previous_position[i] = current_position;
                has_previous_position[i] = true;
            }

            /* Store raw sensor GPIO levels for diagnostics and safety logic. */
            motors[i].upstream_sensor = upstream_sensor;
            motors[i].downstream_sensor = downstream_sensor;

            if (motor_mutex != NULL) {
                xSemaphoreGive(motor_mutex);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(APP_MOTOR_CONTROL_PERIOD_MS));
    }
}

/* Returns a stable configured motor id pointer for static app lifetime. */
esp_err_t hardware_get_motor_id(int index, const char **out_motor_id)
{
    if (out_motor_id == NULL || index < 0 || index >= APP_MOTOR_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_motor_id = motors[index].id;
    return ESP_OK;
}

/* Reads both physical sensor levels as one protected snapshot. */
esp_err_t hardware_get_sensors(const char *motor_id, int *out_upstream_sensor, int *out_downstream_sensor)
{
    motor_t *motor = NULL;

    if (motor_id == NULL || out_upstream_sensor == NULL || out_downstream_sensor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    for (int i = 0; i < APP_MOTOR_COUNT; i++) {
        if (strcmp(motors[i].id, motor_id) == 0) {
            motor = &motors[i];
            break;
        }
    }
    if (motor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (motor_mutex != NULL) {
        xSemaphoreTake(motor_mutex, portMAX_DELAY);
    }

    *out_upstream_sensor = motor->upstream_sensor;
    *out_downstream_sensor = motor->downstream_sensor;

    if (motor_mutex != NULL) {
        xSemaphoreGive(motor_mutex);
    }

    return ESP_OK;
}

/* Sets one BTS7960 direction and PWM; only one side receives duty at a time. */
esp_err_t set_motor(const char *motor_id, int pwm, int direction)
{
    motor_t *motor = NULL;
    int32_t configured_max = HARDWARE_PWM_MAX_DUTY;
    int duty = pwm;
    int output_direction = direction;

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
    if (direction != APP_MOTOR_NEGATIVE_DIR_LEVEL && direction != APP_MOTOR_POSITIVE_DIR_LEVEL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (motor_mutex != NULL) {
        xSemaphoreTake(motor_mutex, portMAX_DELAY);
    }
    /* Read the runtime flip under the shared mutex so command-time reversals affect every caller consistently. */
    if (s_direction_flipped) {
        output_direction = direction == APP_MOTOR_POSITIVE_DIR_LEVEL ?
                           APP_MOTOR_NEGATIVE_DIR_LEVEL : APP_MOTOR_POSITIVE_DIR_LEVEL;
    }
    if (motor_mutex != NULL) {
        xSemaphoreGive(motor_mutex);
    }

    /* Clear both sides before applying direction to avoid hard reversals. */
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, motor->rpwm_ledc_channel, 0),
                        "hardware", "clear rpwm before direction");
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, motor->lpwm_ledc_channel, 0),
                        "hardware", "clear lpwm before direction");
    ESP_RETURN_ON_ERROR(ledc_update_duty(LEDC_LOW_SPEED_MODE, motor->rpwm_ledc_channel),
                        "hardware", "apply rpwm clear");
    ESP_RETURN_ON_ERROR(ledc_update_duty(LEDC_LOW_SPEED_MODE, motor->lpwm_ledc_channel),
                        "hardware", "apply lpwm clear");
    if (output_direction == APP_MOTOR_POSITIVE_DIR_LEVEL) {
        ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, motor->rpwm_ledc_channel, duty),
                            "hardware", "set rpwm duty");
        ESP_RETURN_ON_ERROR(ledc_update_duty(LEDC_LOW_SPEED_MODE, motor->rpwm_ledc_channel),
                            "hardware", "apply rpwm duty");
    } else {
        ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, motor->lpwm_ledc_channel, duty),
                            "hardware", "set lpwm duty");
        ESP_RETURN_ON_ERROR(ledc_update_duty(LEDC_LOW_SPEED_MODE, motor->lpwm_ledc_channel),
                            "hardware", "apply lpwm duty");
    }

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

/* Toggles the runtime electrical direction map and reapplies active outputs immediately. */
esp_err_t hardware_flip_direction(bool *out_flipped)
{
    int pwm_snapshot[APP_MOTOR_COUNT] = {0};
    int direction_snapshot[APP_MOTOR_COUNT] = {0};
    bool flipped = false;

    if (out_flipped == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (motor_mutex != NULL) {
        xSemaphoreTake(motor_mutex, portMAX_DELAY);
    }
    /* Toggle once in hardware.c so console, MQTT, PID, and state-machine calls all share one direction map. */
    s_direction_flipped = !s_direction_flipped;
    flipped = s_direction_flipped;
    for (int i = 0; i < APP_MOTOR_COUNT; i++) {
        /* Snapshot current logical outputs so moving conveyors reverse as soon as the flip command runs. */
        pwm_snapshot[i] = motors[i].pwm;
        direction_snapshot[i] = motors[i].direction;
    }
    if (motor_mutex != NULL) {
        xSemaphoreGive(motor_mutex);
    }

    for (int i = 0; i < APP_MOTOR_COUNT; i++) {
        if (pwm_snapshot[i] > 0) {
            const esp_err_t err = set_motor(motors[i].id, pwm_snapshot[i], direction_snapshot[i]);

            if (err != ESP_OK) {
                return err;
            }
        }
    }

    *out_flipped = flipped;
    return ESP_OK;
}

/* Reports the runtime direction map so job logic can match sensor order to the active travel direction. */
esp_err_t hardware_get_direction_flipped(bool *out_flipped)
{
    if (out_flipped == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (motor_mutex != NULL) {
        xSemaphoreTake(motor_mutex, portMAX_DELAY);
    }
    /* Read under the same mutex used by flip_direction so a job starts with one stable direction map. */
    *out_flipped = s_direction_flipped;
    if (motor_mutex != NULL) {
        xSemaphoreGive(motor_mutex);
    }

    return ESP_OK;
}

/* Stops one motor by cutting both BTS7960 PWM inputs while preserving the last direction value. */
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

    /* Cut both hardware duties immediately before updating shared state. */
    (void)ledc_set_duty(LEDC_LOW_SPEED_MODE, motor->rpwm_ledc_channel, 0);
    (void)ledc_set_duty(LEDC_LOW_SPEED_MODE, motor->lpwm_ledc_channel, 0);
    (void)ledc_update_duty(LEDC_LOW_SPEED_MODE, motor->rpwm_ledc_channel);
    (void)ledc_update_duty(LEDC_LOW_SPEED_MODE, motor->lpwm_ledc_channel);

    if (motor_mutex != NULL) {
        xSemaphoreTake(motor_mutex, portMAX_DELAY);
    }

    motor->pwm = 0;

    if (motor_mutex != NULL) {
        xSemaphoreGive(motor_mutex);
    }
}
