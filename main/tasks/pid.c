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

/* Clears PID history only; gains/target survive resets because callers use this after mode or tuning changes. */
static esp_err_t reset_pid_memory(const char *motor_id)
{
    motor_t *motor = NULL;

    if (motor_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

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

    motor->integral = 0.0f;
    motor->previous_error = 0.0f;
    motor->has_previous_error = false;

    if (motor_mutex != NULL) {
        xSemaphoreGive(motor_mutex);
    }

    return ESP_OK;
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

/* Clamps the integral accumulator to the largest useful PWM contribution. */
static float clamp_integral(float integral, float ki, int32_t max_pwm)
{
    if (ki <= 0.0f) {
        return 0.0f;
    }

    /* Limit windup so integral output cannot exceed the configured PWM range. */
    const float limit = (float)max_pwm / ki;
    if (integral > limit) {
        return limit;
    }
    if (integral < -limit) {
        return -limit;
    }

    return integral;
}

/* Boot init only clears runtime history; per-motor gains stay in motor_t and will be restored by NVS later. */
esp_err_t motor_pid_init(const char *motor_id)
{
    return reset_pid_memory(motor_id);
}

/* PID gains live here instead of runtime_config so each motor can be tuned independently. */
esp_err_t set_pid_gains(const char *motor_id, int kp_milli, int ki_milli, int kd_milli)
{
    motor_t *motor = NULL;

    if (motor_id == NULL || kp_milli < 0 || ki_milli < 0 || kd_milli < 0) {
        return ESP_ERR_INVALID_ARG;
    }

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

    /* Reset PID history because old integral/derivative state belongs to the previous tuning. */
    motor->kp = (float)kp_milli / PID_GAIN_SCALE;
    motor->ki = (float)ki_milli / PID_GAIN_SCALE;
    motor->kd = (float)kd_milli / PID_GAIN_SCALE;
    motor->integral = 0.0f;
    motor->previous_error = 0.0f;
    motor->has_previous_error = false;

    if (motor_mutex != NULL) {
        xSemaphoreGive(motor_mutex);
    }

    return ESP_OK;
}

/* Returns milli-units to keep console/NVS integer-friendly while PID math uses floats internally. */
esp_err_t get_pid_gains(const char *motor_id, int *out_kp_milli, int *out_ki_milli, int *out_kd_milli)
{
    motor_t *motor = NULL;

    if (motor_id == NULL || out_kp_milli == NULL || out_ki_milli == NULL || out_kd_milli == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

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

    *out_kp_milli = (int)(motor->kp * PID_GAIN_SCALE);
    *out_ki_milli = (int)(motor->ki * PID_GAIN_SCALE);
    *out_kd_milli = (int)(motor->kd * PID_GAIN_SCALE);

    if (motor_mutex != NULL) {
        xSemaphoreGive(motor_mutex);
    }

    return ESP_OK;
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

    if (!motor->position_control) {
        if (motor_mutex != NULL) {
            xSemaphoreGive(motor_mutex);
        }
        return ESP_ERR_INVALID_STATE;
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

/* Runs one motor's position PID loop and writes signed PWM through set_motor(). */
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
        int32_t position_tolerance = 0;

        /* Read all shared inputs once so this loop iteration uses a consistent view. */
        if (read_pid_snapshot(motor_id, &snapshot) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(APP_MOTOR_CONTROL_PERIOD_MS));
            continue;
        }

        /* Only shared clamps come from runtime_config; gains are per-motor values from the snapshot above. */
        (void)runtime_config_get(RUNTIME_CONFIG_MAX_PWM, &max_pwm);
        (void)runtime_config_get(RUNTIME_CONFIG_POSITION_TOLERANCE_COUNTS, &position_tolerance);
        if (max_pwm < 0) {
            max_pwm = 0;
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

        const float dt_seconds = (float)APP_MOTOR_CONTROL_PERIOD_MS / 1000.0f;
        const int raw_error_counts = snapshot.target_position - snapshot.current_position;
        int pid_error_counts = 0;

        /* Tolerance is a PID deadband, not an early stop, so controller history stays coherent near target. */
        if (int_abs(raw_error_counts) > position_tolerance) {
            pid_error_counts = raw_error_counts > 0 ?
                               raw_error_counts - position_tolerance : raw_error_counts + position_tolerance;
        }

        const float error = (float)pid_error_counts;
        const float derivative = snapshot.has_previous_error ?
                                 (error - snapshot.previous_error) / dt_seconds : 0.0f;

        const float integral = pid_error_counts == 0 ? 0.0f :
                               clamp_integral(snapshot.integral + (error * dt_seconds), snapshot.ki, max_pwm);
        float pwm_output = (snapshot.kp * error) +
                           (snapshot.ki * integral) +
                           (snapshot.kd * derivative);
        if (pwm_output > (float)max_pwm) {
            pwm_output = (float)max_pwm;
        }
        if (pwm_output < (float)-max_pwm) {
            pwm_output = (float)-max_pwm;
        }

        /* PID output is signed PWM directly; no fake speed command layer sits between PID and hardware. */
        const int signed_pwm = (int)pwm_output;
        const int direction = signed_pwm == 0 ? snapshot.direction :
                              signed_pwm > 0 ? APP_MOTOR_POSITIVE_DIR_LEVEL : APP_MOTOR_NEGATIVE_DIR_LEVEL;

        (void)set_motor(motor_id, int_abs(signed_pwm), direction);
        (void)write_pid_memory(motor_id, integral, error, true);

        vTaskDelay(pdMS_TO_TICKS(APP_MOTOR_CONTROL_PERIOD_MS));
    }
}
