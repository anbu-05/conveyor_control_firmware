#pragma once

/*
 * Hardware service for the single axis.
 * main.c creates hardware_task(), while this module initializes motor PWM,
 * encoder PCNT, physical sensors, and exposes raw motor control.
 */

#include "esp_err.h"
#include "shared/app_state.h"

/* Initializes direction GPIO and LEDC PWM generation for the motor driver. */
esp_err_t hardware_motor_init(void);

/* Initializes PCNT quadrature counting for the motor encoder. */
esp_err_t hardware_encoder_init(void);

/* Initializes the positive and negative physical sensor GPIO inputs. */
esp_err_t hardware_sensor_init(void);

/* Initializes all hardware owned by this service. */
esp_err_t hardware_init(void);

/* Polls hardware state forever after main.c creates this task. */
void hardware_task(void *arg);

/* Applies raw motor output requested by PID or guarded diagnostics. */
esp_err_t set_motor(int direction, int pwm);

/* Cuts PWM immediately and records the stopped output in app_state. */
void stop_motor(void);
