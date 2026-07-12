#pragma once

/*
 * Shared application state storage.
 *
 * This header intentionally exposes motors[] as a C compromise: app_state is
 * the storage warehouse, while each owning task/module exposes behavior-focused
 * APIs for the fields it owns. Prefer hardware.h for hardware-owned fields and
 * pid.h for PID-owned fields instead of reaching across ownership boundaries.
 *
 * C++ could enforce this boundary with private storage, const views, friend
 * owners, and RAII locking. In C, the boundary is documented and kept clean by
 * convention. Take motor_mutex before direct multi-field reads or writes so
 * snapshots stay consistent across tasks.
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

typedef enum {
    /* Keep controller type explicit because PID_control now only means PID owns output. */
    MOTOR_PID_MODE_POSITION,
    MOTOR_PID_MODE_SPEED,
} motor_pid_mode_t;

typedef struct {
    /* Stable user-facing id used by console, hardware APIs, and PID APIs. */
    char id[APP_MOTOR_ID_MAX_LEN];

    /* Current motor output requested by set_motor(). */
    int pwm;
    int direction;

    /* Encoder-derived position/speed data share this block because hardware_task publishes both snapshots. */
    int encoder_count;
    int current_position;
    int target_position;
    int speed;
    int target_speed;
    int position_offset;
    bool PID_control;
    motor_pid_mode_t pid_mode;

    /* Per-motor PID gains; global config was removed because motors may tune differently. TODO: persist in NVS and load during boot. */
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
