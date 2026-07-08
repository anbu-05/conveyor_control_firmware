#pragma once

/*
 * PID motor-control task public API.
 * The PID task owns repetitive target tracking and PWM decisions so the state
 * machine can request intent without writing motor output every control tick.
 */

#include "esp_err.h"

/* Initializes PID gains from runtime config; main.c starts the task. */
esp_err_t motor_pid_init(void);

/* Runs the periodic position PID loop after main.c creates this task. */
void motor_pid_task(void *arg);

/* Sets the requested offset-corrected target position without changing control mode. */
esp_err_t set_position(int target_position);

/* Returns the latest offset-corrected position published by hardware_task(). */
int get_position(void);

/* Sets the offset between encoder zero and the real axis position. */
esp_err_t set_offset(int position_offset);

/* Updates live PID gains; persistence remains owned by runtime_config. */
esp_err_t setk(float kp, float ki, float kd);
