/*
 * PID motor-control task.
 * Each task instance owns one motor id and converts that motor's target fields
 * into direction/PWM output without requiring callers to message the task.
 */

#include "tasks/pid.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "config/config.h"
#include "config/runtime_config.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "shared/app_state.h"
#include "tasks/hardware.h"

#define PID_GAIN_SCALE 1000.0f

typedef struct {
    int current_position;
    int target_position;
    int target_speed;
    int direction;
    bool position_control;
    float kp;
    float ki;
    float kd;
    float integral;
    float previous_error;
    bool has_previous_error;
} pid_snapshot_t;

/* Returns an integer absolute value without pulling in extra math helpers. */
static int int_abs(int value)
{
    return value < 0 ? -value : value;
}

/* Converts runtime-config milli-unit gains into live float PID gains. */
static float config_gain_or_zero(runtime_config_key_t key)
{
    int32_t value = 0;

    /* Runtime config may fail during early bring-up; default to a safe zero gain. */
    if (runtime_config_get(key, &value) != ESP_OK) {
        return 0.0f;
    }

    return (float)value / PID_GAIN_SCALE;
}

/* Reads one motor's PID-relevant fields as a consistent snapshot. */
static esp_err_t read_pid_snapshot(const char *motor_id, pid_snapshot_t *out_snapshot)
{
    motor_t *motor = NULL;

    if (motor_id == NULL || out_snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Find the motor matching this PID task instance's configured id. */
    for (int i = 0; i < APP_MOTOR_COUNT; i++) {
        if (strcmp(motors[i].id, motor_id) == 0) {
            motor = &motors[i];
            break;
        }
    }
    if (motor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (motor_mutex != NULL) {
        xSemaphoreTake(motor_mutex, portMAX_DELAY);
    }

    /* Copy all inputs used by one PID tick so calculations are self-consistent. */
    out_snapshot->current_position = motor->current_position;
    out_snapshot->target_position = motor->target_position;
    out_snapshot->target_speed = motor->target_speed;
    out_snapshot->direction = motor->direction;
    out_snapshot->position_control = motor->position_control;
    out_snapshot->kp = motor->kp;
    out_snapshot->ki = motor->ki;
    out_snapshot->kd = motor->kd;
    out_snapshot->integral = motor->integral;
    out_snapshot->previous_error = motor->previous_error;
    out_snapshot->has_previous_error = motor->has_previous_error;

    if (motor_mutex != NULL) {
        xSemaphoreGive(motor_mutex);
    }

    return ESP_OK;
}

/* Writes one motor's PID runtime memory after a control tick. */
static esp_err_t write_pid_memory(const char *motor_id,
                                  float integral,
                                  float previous_error,
                                  bool has_previous_error)
{
    motor_t *motor = NULL;

    if (motor_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Find the motor whose runtime PID memory should be updated. */
    for (int i = 0; i < APP_MOTOR_COUNT; i++) {
        if (strcmp(motors[i].id, motor_id) == 0) {
            motor = &motors[i];
            break;
        }
    }
    if (motor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (motor_mutex != NULL) {
        xSemaphoreTake(motor_mutex, portMAX_DELAY);
    }

    /* Persist only PID runtime memory; public target/gain fields are left alone. */
    motor->integral = integral;
    motor->previous_error = previous_error;
    motor->has_previous_error = has_previous_error;

    if (motor_mutex != NULL) {
        xSemaphoreGive(motor_mutex);
    }

    return ESP_OK;
}

/* Clamps the integral accumulator to the largest useful speed contribution. */
static float clamp_integral(float integral, float ki, int32_t max_speed_counts_per_sec)
{
    if (ki <= 0.0f) {
        return 0.0f;
    }

    /* Limit windup so integral output cannot exceed the configured speed range. */
    const float limit = (float)max_speed_counts_per_sec / ki;
    if (integral > limit) {
        return limit;
    }
    if (integral < -limit) {
        return -limit;
    }

    return integral;
}

/* Clamps signed speed to the configured counts/sec range. */
static int clamp_speed(float speed_counts_per_sec, int32_t max_speed_counts_per_sec)
{
    /* Positive and negative speed limits are symmetric around zero. */
    if (speed_counts_per_sec > (float)max_speed_counts_per_sec) {
        return (int)max_speed_counts_per_sec;
    }
    if (speed_counts_per_sec < (float)-max_speed_counts_per_sec) {
        return (int)-max_speed_counts_per_sec;
    }

    return (int)speed_counts_per_sec;
}

/* Converts signed speed in encoder counts/sec to direction and PWM for one motor. */
static esp_err_t set_motor_speed(const char *motor_id,
                                 int speed_counts_per_sec,
                                 int current_direction,
                                 int32_t max_speed_counts_per_sec,
                                 int32_t max_pwm)
{
    if (max_speed_counts_per_sec <= 0 || max_pwm <= 0) {
        return set_motor(motor_id, 0, current_direction);
    }

    /* Clamp speed before deriving direction and PWM magnitude. */
    int clamped_speed = speed_counts_per_sec;
    if (clamped_speed > max_speed_counts_per_sec) {
        clamped_speed = (int)max_speed_counts_per_sec;
    }
    if (clamped_speed < -max_speed_counts_per_sec) {
        clamped_speed = (int)-max_speed_counts_per_sec;
    }

    /* Positive speed maps to positive direction; zero preserves current direction. */
    const int direction = clamped_speed == 0 ? current_direction :
                          clamped_speed > 0 ? APP_MOTOR_POSITIVE_DIR_LEVEL : APP_MOTOR_NEGATIVE_DIR_LEVEL;
    const int speed_magnitude = int_abs(clamped_speed);
    const int pwm = (int)(((int64_t)speed_magnitude * max_pwm) / max_speed_counts_per_sec);

    return set_motor(motor_id, pwm, direction);
}

/* Initializes one motor's live PID gains from already-loaded runtime config values. */
esp_err_t motor_pid_init(const char *motor_id)
{
    /* Reuse the public gain API so validation and mutex behavior stay centralized. */
    return setk(motor_id,
                config_gain_or_zero(RUNTIME_CONFIG_PID_KP_MILLI),
                config_gain_or_zero(RUNTIME_CONFIG_PID_KI_MILLI),
                config_gain_or_zero(RUNTIME_CONFIG_PID_KD_MILLI));
}

/* Sets one motor's offset-corrected position target without changing control mode. */
esp_err_t set_position(const char *motor_id, int target_position)
{
    motor_t *motor = NULL;

    if (motor_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Find the motor whose target should be updated. */
    for (int i = 0; i < APP_MOTOR_COUNT; i++) {
        if (strcmp(motors[i].id, motor_id) == 0) {
            motor = &motors[i];
            break;
        }
    }
    if (motor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (motor_mutex != NULL) {
        xSemaphoreTake(motor_mutex, portMAX_DELAY);
    }

    /* Publish intent; the PID task for this motor consumes it on its next tick. */
    motor->target_position = target_position;

    if (motor_mutex != NULL) {
        xSemaphoreGive(motor_mutex);
    }

    return ESP_OK;
}

/* Returns one motor's latest offset-corrected position. */
esp_err_t get_position(const char *motor_id, int *out_position)
{
    motor_t *motor = NULL;

    if (motor_id == NULL || out_position == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Find the motor whose hardware-published position should be read. */
    for (int i = 0; i < APP_MOTOR_COUNT; i++) {
        if (strcmp(motors[i].id, motor_id) == 0) {
            motor = &motors[i];
            break;
        }
    }
    if (motor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (motor_mutex != NULL) {
        xSemaphoreTake(motor_mutex, portMAX_DELAY);
    }

    /* Return the offset-corrected position, not raw encoder count. */
    *out_position = motor->current_position;

    if (motor_mutex != NULL) {
        xSemaphoreGive(motor_mutex);
    }

    return ESP_OK;
}

/* Updates one motor's real-position offset and republishes current_position. */
esp_err_t set_offset(const char *motor_id, int position_offset)
{
    motor_t *motor = NULL;

    if (motor_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Find the motor whose encoder-to-position offset should be updated. */
    for (int i = 0; i < APP_MOTOR_COUNT; i++) {
        if (strcmp(motors[i].id, motor_id) == 0) {
            motor = &motors[i];
            break;
        }
    }
    if (motor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (motor_mutex != NULL) {
        xSemaphoreTake(motor_mutex, portMAX_DELAY);
    }

    /* Keep current_position immediately consistent with the new offset. */
    motor->position_offset = position_offset;
    motor->current_position = motor->encoder_count + motor->position_offset;

    if (motor_mutex != NULL) {
        xSemaphoreGive(motor_mutex);
    }

    return ESP_OK;
}

/* Updates one motor's live PID gains. Persisting tuning values is handled outside PID. */
esp_err_t setk(const char *motor_id, float kp, float ki, float kd)
{
    motor_t *motor = NULL;

    if (motor_id == NULL || kp < 0.0f || ki < 0.0f || kd < 0.0f) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Find the motor whose live gain values should change. */
    for (int i = 0; i < APP_MOTOR_COUNT; i++) {
        if (strcmp(motors[i].id, motor_id) == 0) {
            motor = &motors[i];
            break;
        }
    }
    if (motor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (motor_mutex != NULL) {
        xSemaphoreTake(motor_mutex, portMAX_DELAY);
    }

    /* Store live-only gains and reset PID memory to avoid mixing old tuning state. */
    motor->kp = kp;
    motor->ki = ki;
    motor->kd = kd;
    motor->integral = 0.0f;
    motor->previous_error = 0.0f;
    motor->has_previous_error = false;

    if (motor_mutex != NULL) {
        xSemaphoreGive(motor_mutex);
    }

    return ESP_OK;
}

/* Runs one motor's PID loop and writes speed-derived output through set_motor(). */
void motor_pid_task(void *arg)
{
    const char *motor_id = (const char *)arg;

    if (motor_id == NULL) {
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        pid_snapshot_t snapshot = {0};
        int32_t max_pwm = 0;
        int32_t max_speed_counts_per_sec = 0;
        int32_t position_tolerance = 0;

        /* Read all shared inputs once so this loop iteration uses a consistent view. */
        if (read_pid_snapshot(motor_id, &snapshot) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(APP_MOTOR_CONTROL_PERIOD_MS));
            continue;
        }

        /* Runtime limits can change while the task runs, so refresh them each tick. */
        (void)runtime_config_get(RUNTIME_CONFIG_MAX_PWM, &max_pwm);
        (void)runtime_config_get(RUNTIME_CONFIG_MAX_SPEED_COUNTS_PER_SEC, &max_speed_counts_per_sec);
        (void)runtime_config_get(RUNTIME_CONFIG_POSITION_TOLERANCE_COUNTS, &position_tolerance);
        if (max_pwm < 0) {
            max_pwm = 0;
        }
        if (max_speed_counts_per_sec < 0) {
            max_speed_counts_per_sec = 0;
        }
        if (position_tolerance < 0) {
            position_tolerance = 0;
        }

        if (!snapshot.position_control) {
            /* Manual/raw mode leaves motor output to explicit hardware API calls. */
            (void)write_pid_memory(motor_id, 0.0f, 0.0f, false);

            vTaskDelay(pdMS_TO_TICKS(APP_MOTOR_CONTROL_PERIOD_MS));
            continue;
        }

        const int error_counts = snapshot.target_position - snapshot.current_position;
        if (int_abs(error_counts) <= position_tolerance) {
            /* We are within tolerance; reset PID memory and command zero speed. */
            (void)write_pid_memory(motor_id, 0.0f, 0.0f, false);
            (void)set_motor_speed(motor_id, 0, snapshot.direction, max_speed_counts_per_sec, max_pwm);
            vTaskDelay(pdMS_TO_TICKS(APP_MOTOR_CONTROL_PERIOD_MS));
            continue;
        }

        const float dt_seconds = (float)APP_MOTOR_CONTROL_PERIOD_MS / 1000.0f;
        const float error = (float)error_counts;
        const float derivative = snapshot.has_previous_error ?
                                 (error - snapshot.previous_error) / dt_seconds : 0.0f;

        /* Integrate error over time, then clamp to prevent windup at speed limits. */
        const float integral = clamp_integral(snapshot.integral + (error * dt_seconds),
                                             snapshot.ki,
                                             max_speed_counts_per_sec);

        /* The PID result is signed speed in counts/sec; conversion happens later. */
        const float speed_output = (snapshot.kp * error) +
                                   (snapshot.ki * integral) +
                                   (snapshot.kd * derivative);
        int target_speed_counts_per_sec = clamp_speed(speed_output, max_speed_counts_per_sec);
        if ((error_counts > 0 && target_speed_counts_per_sec < 0) ||
            (error_counts < 0 && target_speed_counts_per_sec > 0)) {
            target_speed_counts_per_sec = 0;
        }

        /* Convert signed speed to direction/PWM and retain error for derivative. */
        (void)set_motor_speed(motor_id, target_speed_counts_per_sec, snapshot.direction,
                              max_speed_counts_per_sec, max_pwm);
        (void)write_pid_memory(motor_id, integral, error, true);

        vTaskDelay(pdMS_TO_TICKS(APP_MOTOR_CONTROL_PERIOD_MS));
    }
}
