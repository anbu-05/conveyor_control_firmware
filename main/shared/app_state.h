#pragma once

/*
 * Shared application state.
 * axis is intentionally exposed so tasks can update the fields they own
 * without adding one app_state helper per value. Take axis_mutex before direct
 * multi-field reads or writes so snapshots stay consistent across tasks.
 */

#include <stdbool.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/pulse_cnt.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define APP_AXIS_COUNT 1

typedef struct {
    const char *name;

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

    /* Pin and peripheral handles stay beside the runtime values they describe. */
    gpio_num_t dir_gpio;
    gpio_num_t pwm_gpio;
    gpio_num_t encoder_a_gpio;
    gpio_num_t encoder_b_gpio;
    gpio_num_t positive_sensor_gpio;
    gpio_num_t negative_sensor_gpio;
    ledc_channel_t pwm_ledc_channel;
    pcnt_unit_handle_t pcnt_unit;

    /* Direct GPIO levels from the two physical sensors. */
    int positive_sensor;
    int negative_sensor;
} axis_t;

extern SemaphoreHandle_t axis_mutex;
extern axis_t axis;

/* Initializes the protected shared status snapshot before producer tasks start. */
esp_err_t app_state_init(void);
