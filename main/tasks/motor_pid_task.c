/*
 * PID motor-control task.
 * This task owns repetitive position-control output so the state machine can set
 * a target once without writing direction/PWM every control tick.
 */

#include "tasks/motor_pid_task.h"

#include <stdbool.h>
#include <stdint.h>

#include "config/config.h"
#include "config/runtime_config.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "shared/app_state.h"
#include "tasks/hardware_task.h"

#define PID_GAIN_SCALE 1000.0f

typedef struct
{
    int current_position;
    int target_position;
    int target_speed;
    int direction;
    int pwm;
    bool position_control;
    float kp;
    float ki;
    float kd;
} pid_snapshot_t;

static float s_integral;
static float s_previous_error;
static bool s_has_previous_error;

/* Returns an integer absolute value without pulling in extra math helpers. */
static int int_abs(int value)
{
    return value < 0 ? -value : value;
}

/* Converts runtime-config milli-unit gains into live float PID gains. */
static float config_gain_or_zero(runtime_config_key_t key)
{
    int32_t value = 0;
    if (runtime_config_get(key, &value) != ESP_OK)
    {
        return 0.0f;
    }

    return (float)value / PID_GAIN_SCALE;
}

/* Reads the PID-relevant axis fields as one consistent snapshot. */
static pid_snapshot_t read_pid_snapshot(void)
{
    pid_snapshot_t snapshot = {0};

    if (axis_mutex != NULL)
    {
        xSemaphoreTake(axis_mutex, portMAX_DELAY);
    }

    snapshot.current_position = axis.current_position;
    snapshot.target_position = axis.target_position;
    snapshot.target_speed = axis.target_speed;
    snapshot.direction = axis.direction;
    snapshot.pwm = axis.pwm;
    snapshot.position_control = axis.position_control;
    snapshot.kp = axis.kp;
    snapshot.ki = axis.ki;
    snapshot.kd = axis.kd;

    if (axis_mutex != NULL)
    {
        xSemaphoreGive(axis_mutex);
    }

    return snapshot;
}

/* Clamps the integral accumulator to the largest useful speed contribution. */
static float clamp_integral(float integral, float ki, int32_t max_speed_counts_per_sec)
{
    if (ki <= 0.0f)
    {
        return 0.0f;
    }

    const float limit = (float)max_speed_counts_per_sec / ki;
    if (integral > limit)
    {
        return limit;
    }
    if (integral < -limit)
    {
        return -limit;
    }

    return integral;
}

/* Clamps signed speed to the configured counts/sec range. */
static int clamp_speed(float speed_counts_per_sec, int32_t max_speed_counts_per_sec)
{
    if (speed_counts_per_sec > (float)max_speed_counts_per_sec)
    {
        return (int)max_speed_counts_per_sec;
    }
    if (speed_counts_per_sec < (float)-max_speed_counts_per_sec)
    {
        return (int)-max_speed_counts_per_sec;
    }

    return (int)speed_counts_per_sec;
}

/* Converts signed speed in encoder counts/sec to direction and PWM. */
static esp_err_t set_motor_speed(int speed_counts_per_sec,
                                 int current_direction,
                                 int32_t max_speed_counts_per_sec,
                                 int32_t max_pwm)
{
    if (max_speed_counts_per_sec <= 0 || max_pwm <= 0)
    {
        return set_motor(current_direction, 0);
    }

    int clamped_speed = speed_counts_per_sec;
    if (clamped_speed > max_speed_counts_per_sec)
    {
        clamped_speed = (int)max_speed_counts_per_sec;
    }
    if (clamped_speed < -max_speed_counts_per_sec)
    {
        clamped_speed = (int)-max_speed_counts_per_sec;
    }

    const int direction = clamped_speed == 0 ? current_direction :
                          clamped_speed > 0 ? APP_AXIS_POSITIVE_DIR_LEVEL : APP_AXIS_NEGATIVE_DIR_LEVEL;
    const int speed_magnitude = int_abs(clamped_speed);
    const int pwm = (int)(((int64_t)speed_magnitude * max_pwm) / max_speed_counts_per_sec);

    return set_motor(direction, pwm);
}

/* Initializes live PID gains from already-loaded runtime config values. */
esp_err_t motor_pid_init(void)
{
    return setk(config_gain_or_zero(RUNTIME_CONFIG_PID_KP_MILLI),
                config_gain_or_zero(RUNTIME_CONFIG_PID_KI_MILLI),
                config_gain_or_zero(RUNTIME_CONFIG_PID_KD_MILLI));
}

/* Sets the offset-corrected position target without changing control mode. */
esp_err_t set_position(int target_position)
{
    if (axis_mutex != NULL)
    {
        xSemaphoreTake(axis_mutex, portMAX_DELAY);
    }

    axis.target_position = target_position;

    if (axis_mutex != NULL)
    {
        xSemaphoreGive(axis_mutex);
    }

    return ESP_OK;
}

/* Returns the latest offset-corrected position. */
int get_position(void)
{
    int position = 0;

    if (axis_mutex != NULL)
    {
        xSemaphoreTake(axis_mutex, portMAX_DELAY);
    }

    position = axis.current_position;

    if (axis_mutex != NULL)
    {
        xSemaphoreGive(axis_mutex);
    }

    return position;
}

/* Updates the real-position offset and immediately republishes current_position. */
esp_err_t set_offset(int position_offset)
{
    if (axis_mutex != NULL)
    {
        xSemaphoreTake(axis_mutex, portMAX_DELAY);
    }

    axis.position_offset = position_offset;
    axis.current_position = axis.encoder_count + axis.position_offset;

    if (axis_mutex != NULL)
    {
        xSemaphoreGive(axis_mutex);
    }

    return ESP_OK;
}

/* Updates live PID gains. Persisting tuning values is handled outside PID. */
esp_err_t setk(float kp, float ki, float kd)
{
    if (kp < 0.0f || ki < 0.0f || kd < 0.0f)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (axis_mutex != NULL)
    {
        xSemaphoreTake(axis_mutex, portMAX_DELAY);
    }

    axis.kp = kp;
    axis.ki = ki;
    axis.kd = kd;

    if (axis_mutex != NULL)
    {
        xSemaphoreGive(axis_mutex);
    }

    return ESP_OK;
}

/* Runs position PID and writes speed-derived motor output through set_motor(). */
void motor_pid_task(void *arg)
{
    (void)arg;

    while (true)
    {
        /* Read all shared inputs once so this loop iteration uses a consistent view. */
        pid_snapshot_t snapshot = read_pid_snapshot();
        int32_t max_pwm = 0;
        int32_t max_speed_counts_per_sec = 0;
        int32_t position_tolerance = 0;

        /* Runtime limits can change while the task runs, so refresh them each tick. */
        (void)runtime_config_get(RUNTIME_CONFIG_MAX_PWM, &max_pwm);
        (void)runtime_config_get(RUNTIME_CONFIG_MAX_SPEED_COUNTS_PER_SEC, &max_speed_counts_per_sec);
        (void)runtime_config_get(RUNTIME_CONFIG_POSITION_TOLERANCE_COUNTS, &position_tolerance);
        if (max_pwm < 0)
        {
            max_pwm = 0;
        }
        if (max_speed_counts_per_sec < 0)
        {
            max_speed_counts_per_sec = 0;
        }
        if (position_tolerance < 0)
        {
            position_tolerance = 0;
        }

        if (!snapshot.position_control)
        {
            /* Manual mode sends signed target speed directly through the mapper. */
            s_integral = 0.0f;
            s_previous_error = 0.0f;
            s_has_previous_error = false;

            (void)set_motor_speed(snapshot.target_speed, snapshot.direction, max_speed_counts_per_sec, max_pwm);

            vTaskDelay(pdMS_TO_TICKS(APP_AXIS_CONTROL_PERIOD_MS));
            continue;
        }

        const int error_counts = snapshot.target_position - snapshot.current_position;
        if (int_abs(error_counts) <= position_tolerance)
        {
            /* We are within tolerance; hold PID state reset and command zero speed. */
            s_integral = 0.0f;
            s_previous_error = 0.0f;
            s_has_previous_error = false;
            (void)set_motor_speed(0, snapshot.direction, max_speed_counts_per_sec, max_pwm);
            vTaskDelay(pdMS_TO_TICKS(APP_AXIS_CONTROL_PERIOD_MS));
            continue;
        }

        const float dt_seconds = (float)APP_AXIS_CONTROL_PERIOD_MS / 1000.0f;
        const float error = (float)error_counts;
        const float derivative = s_has_previous_error ? (error - s_previous_error) / dt_seconds : 0.0f;
        /* Integrate error over time, then clamp to prevent windup at speed limits. */
        s_integral = clamp_integral(s_integral + (error * dt_seconds), snapshot.ki, max_speed_counts_per_sec);

        /* The PID result is signed speed in counts/sec; conversion happens in set_motor_speed(). */
        const float speed_output = (snapshot.kp * error) + (snapshot.ki * s_integral) + (snapshot.kd * derivative);
        int target_speed_counts_per_sec = clamp_speed(speed_output, max_speed_counts_per_sec);
        if ((error_counts > 0 && target_speed_counts_per_sec < 0) ||
            (error_counts < 0 && target_speed_counts_per_sec > 0))
        {
            target_speed_counts_per_sec = 0;
        }

        /* Convert signed speed to direction/PWM and retain error for derivative. */
        (void)set_motor_speed(target_speed_counts_per_sec, snapshot.direction, max_speed_counts_per_sec, max_pwm);
        s_previous_error = error;
        s_has_previous_error = true;

        vTaskDelay(pdMS_TO_TICKS(APP_AXIS_CONTROL_PERIOD_MS));
    }
}
