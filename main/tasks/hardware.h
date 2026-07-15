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

/* Returns the configured motor id for an index in the shared motor table. */
esp_err_t hardware_get_motor_id(int index, const char **out_motor_id);

/* Returns one protected snapshot of a motor's physical sensor GPIO levels. */
esp_err_t hardware_get_sensors(const char *motor_id, int *out_upstream_sensor, int *out_downstream_sensor);

/* Applies raw motor output requested by PID or guarded diagnostics. */
esp_err_t set_motor(const char *motor_id, int pwm, int direction);

/* Flips the runtime output map so existing direction constants drive the opposite hardware side. */
esp_err_t hardware_flip_direction(bool *out_flipped);

/* Reads the runtime output map so tray jobs can keep sensor order aligned with travel direction. */
esp_err_t hardware_get_direction_flipped(bool *out_flipped);

/* Cuts PWM immediately and records the stopped output in app_state. */
void stop_motor(const char *motor_id);
