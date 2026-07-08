#pragma once

/*
 * Hardware service for configured motors.
 * main.c creates hardware_task(), while this module initializes motor PWM,
 * encoder PCNT, physical sensors, and exposes raw motor control.
 */

#include "esp_err.h"
#include "shared/app_state.h"

/* Initializes direction GPIO and LEDC PWM generation for one motor driver. */
esp_err_t hardware_motor_init(const char *motor_id);

/* Initializes PCNT quadrature counting for one motor encoder. */
esp_err_t hardware_encoder_init(const char *motor_id);

/* Initializes the positive and negative physical sensor GPIO inputs for one motor. */
esp_err_t hardware_sensor_init(const char *motor_id);

/* Initializes all hardware owned by this service for one motor id. */
esp_err_t hardware_init(const char *motor_id);

/* Polls hardware state forever after main.c creates this task. */
void hardware_task(void *arg);

/* Applies raw motor output requested by PID or guarded diagnostics. */
esp_err_t set_motor(const char *motor_id, int pwm, int direction);

/* Cuts PWM immediately and records the stopped output in app_state. */
void stop_motor(const char *motor_id);
