#pragma once

/*
 * PID motor-control task public API.
 * The PID task owns repetitive target tracking and PWM decisions so the state
 * machine can request intent without writing motor output every control tick.
 */

#include "esp_err.h"
#include "shared/app_state.h"

/* Initializes one motor's PID task state; main.c starts the task. */
esp_err_t motor_pid_init(const char *motor_id, motor_pid_mode_t mode);

/* Runs the periodic position PID loop after main.c creates this task. */
void motor_pid_task(void *arg);

/* Enables or disables PID ownership of one motor's output and resets PID memory. */
esp_err_t pid_set_control(const char *motor_id, bool enabled);

/* Returns whether PID currently owns one motor's output. */
esp_err_t pid_get_control(const char *motor_id, bool *out_enabled);

/* Selects one motor's PID error source and resets PID memory. */
esp_err_t pid_set_mode(const char *motor_id, motor_pid_mode_t mode);

/* Returns one motor's selected PID error source. */
esp_err_t pid_get_mode(const char *motor_id, motor_pid_mode_t *out_mode);

/* Sets one motor's requested offset-corrected target position and selects position PID mode. */
esp_err_t set_position(const char *motor_id, int target_position);

/* Returns one motor's latest offset-corrected position published by hardware_task(). */
esp_err_t get_position(const char *motor_id, int *out_position);

/* Sets one motor's requested speed target and selects speed PID mode. */
esp_err_t set_speed(const char *motor_id, int target_speed);

/* Returns one motor's latest speed snapshot published by hardware_task(). */
esp_err_t get_speed(const char *motor_id, int *out_speed);

/* Sets one motor's per-motor PID gains in milli-units and resets PID memory. */
esp_err_t set_pid_gains(const char *motor_id, int kp_milli, int ki_milli, int kd_milli);

/* Returns one motor's per-motor PID gains in milli-units. */
esp_err_t get_pid_gains(const char *motor_id, int *out_kp_milli, int *out_ki_milli, int *out_kd_milli);

/* Sets one motor's offset between encoder zero and the real motor position. */
esp_err_t set_offset(const char *motor_id, int position_offset);
