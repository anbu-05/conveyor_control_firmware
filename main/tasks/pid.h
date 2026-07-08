#pragma once

/*
 * PID motor-control task public API.
 * The PID task owns repetitive target tracking and PWM decisions so the state
 * machine can request intent without writing motor output every control tick.
 */

#include "esp_err.h"

/* Initializes one motor's PID gains from runtime config; main.c starts the task. */
esp_err_t motor_pid_init(const char *motor_id);

/* Runs the periodic position PID loop after main.c creates this task. */
void motor_pid_task(void *arg);

/* Sets one motor's requested offset-corrected target position without changing control mode. */
esp_err_t set_position(const char *motor_id, int target_position);

/* Returns one motor's latest offset-corrected position published by hardware_task(). */
esp_err_t get_position(const char *motor_id, int *out_position);

/* Sets one motor's offset between encoder zero and the real motor position. */
esp_err_t set_offset(const char *motor_id, int position_offset);

/* Updates one motor's live PID gains; persistence remains owned by runtime_config. */
esp_err_t setk(const char *motor_id, float kp, float ki, float kd);
