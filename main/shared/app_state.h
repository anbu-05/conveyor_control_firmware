#pragma once

/*
 * Shared application state.
 * motors is intentionally exposed so tasks can update the fields they own
 * without adding one app_state helper per value. Take motor_mutex before direct
 * multi-field reads or writes so snapshots stay consistent across tasks.
 */

#include <stdbool.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/pulse_cnt.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define APP_MOTOR_COUNT 1
#define APP_MOTOR_ID_MAX_LEN 8

typedef struct {
    /* Stable user-facing id used by console, hardware APIs, and PID APIs. */
    char id[APP_MOTOR_ID_MAX_LEN];

    /* Current motor output requested by set_motor(). */
    int pwm;
    int direction;

    /* Encoder-derived position data. PCNT remains the hardware source of truth. */
    int encoder_count;
    int current_position;
    int target_position;
    int position_offset;
    int target_speed;
    int current_speed;
    bool position_control;

    /* Live PID tuning values. Runtime config owns persisted/default values. */
    float kp;
    float ki;
    float kd;

    /* Per-motor PID runtime memory owned by that motor's PID task instance. */
    float integral;
    float previous_error;
    bool has_previous_error;

    /* Pin and peripheral handles stay beside the runtime values they describe. */
    gpio_num_t rpwm_gpio;
    gpio_num_t lpwm_gpio;
    gpio_num_t ren_gpio;
    gpio_num_t len_gpio;
    gpio_num_t encoder_a_gpio;
    gpio_num_t encoder_b_gpio;
    gpio_num_t upstream_sensor_gpio;
    gpio_num_t downstream_sensor_gpio;
    ledc_channel_t rpwm_ledc_channel;
    ledc_channel_t lpwm_ledc_channel;
    pcnt_unit_handle_t pcnt_unit;

    /* Direct GPIO levels from the two physical conveyor sensors. */
    int upstream_sensor;
    int downstream_sensor;
} motor_t;

extern SemaphoreHandle_t motor_mutex;
extern motor_t motors[APP_MOTOR_COUNT];

/* Initializes the protected shared status snapshot before producer tasks start. */
esp_err_t app_state_init(void);
