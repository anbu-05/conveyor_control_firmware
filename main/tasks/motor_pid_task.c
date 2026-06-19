#include "app_state.h"

#include <stdint.h>

#include "config.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/pulse_cnt.h"
#include "esp_err.h"
#include "runtime_config.h"

#define SPEED_AVG_SAMPLE_COUNT 5
#define SPEED_D_BASE_DELAY_MS 20

typedef struct {
    int pwm;
    int speed;
} speed_pwm_point_t;

typedef struct {
    int target_speed;
    int speed;
    int error;
    int base_pwm;
    int p_step;
    int d_step;
    int requested_pwm;
    int applied_pwm;
    int direction;
    int kp_milli;
    int kd_milli;
} pid_debug_t;

static const speed_pwm_point_t speed_pwm_table[] = {
    {0, 0},
    {8, 360},
    {16, 1040},
    {24, 1650},
    {32, 2270},
    {48, 3490},
    {64, 4670},
    {72, 5340},
    {80, 6050},
    {88, 6570},
    {96, 7230},
    {104, 7890},
    {112, 8590},
    {120, 9260},
    {128, 9870},
};

static int abs_int(int value)
{
    if (value < 0) {
        return -value;
    }

    return value;
}

static int clamp_int(int value, int minimum, int maximum)
{
    if (value < minimum) {
        return minimum;
    }
    if (value > maximum) {
        return maximum;
    }

    return value;
}

static int speed_to_base_pwm(int target_speed)
{
    int point_count = sizeof(speed_pwm_table) / sizeof(speed_pwm_table[0]);

    if (target_speed <= 0) {
        return 0;
    }

    for (int i = 1; i < point_count; i++) {
        if (target_speed <= speed_pwm_table[i].speed) {
            int low_speed = speed_pwm_table[i - 1].speed;
            int high_speed = speed_pwm_table[i].speed;
            int low_pwm = speed_pwm_table[i - 1].pwm;
            int high_pwm = speed_pwm_table[i].pwm;

            return low_pwm + ((target_speed - low_speed) * (high_pwm - low_pwm)) / (high_speed - low_speed);
        }
    }

    return CONVEYOR_SPEED_PID_PWM_MAX;
}

static int direction_to_sign(int direction)
{
    if (direction == 0) {
        return -1;
    }

    return 1;
}

static void slew_signed_pwm_toward(int *signed_pwm, int requested_signed_pwm)
{
    if (*signed_pwm < requested_signed_pwm) {
        *signed_pwm += CONVEYOR_PWM_SLEW_STEP;
        if (*signed_pwm > requested_signed_pwm) {
            *signed_pwm = requested_signed_pwm;
        }
    } else if (*signed_pwm > requested_signed_pwm) {
        *signed_pwm -= CONVEYOR_PWM_SLEW_STEP;
        if (*signed_pwm < requested_signed_pwm) {
            *signed_pwm = requested_signed_pwm;
        }
    }
}

static void update_motor_speed_control(motor_t *motor, int speed, int *last_error, int *direction, int *pwm,
                                       pid_debug_t *debug)
{
    int target_speed = 0;
    int error = 0;
    int d_error = 0;
    int normalized_d_error = 0;
    int base_pwm = 0;
    int base_pwm_magnitude = 0;
    int p_step = 0;
    int d_step = 0;
    int requested_signed_pwm = 0;
    int current_signed_pwm = 0;
    int kp_milli = runtime_config_speed_kp_milli();
    int kd_milli = runtime_config_speed_kd_milli();
    int current_direction = 0;
    int speed_control = 0;

    xSemaphoreTake(motor_mutex, portMAX_DELAY);
    target_speed = motor->target_speed;
    *pwm = motor->pwm;
    current_direction = motor->direction;
    *direction = current_direction;
    speed_control = motor->speed_control ? 1 : 0;
    xSemaphoreGive(motor_mutex);

    if (!speed_control) {
        *last_error = 0;
        if (debug != NULL) {
            debug->target_speed = target_speed;
            debug->speed = speed;
            debug->error = 0;
            debug->base_pwm = 0;
            debug->p_step = 0;
            debug->d_step = 0;
            debug->requested_pwm = *pwm * direction_to_sign(*direction);
            debug->applied_pwm = *pwm;
            debug->direction = *direction;
            debug->kp_milli = kp_milli;
            debug->kd_milli = kd_milli;
        }
        return;
    }

    current_signed_pwm = *pwm * direction_to_sign(current_direction);
    error = target_speed - speed;
    d_error = error - *last_error;
    normalized_d_error = (d_error * SPEED_D_BASE_DELAY_MS) / MOTOR_PID_DELAY_MS;
    *last_error = error;

    if (target_speed < 0) {
        base_pwm_magnitude = speed_to_base_pwm(abs_int(target_speed));
        base_pwm = -base_pwm_magnitude;
    } else if (target_speed > 0) {
        base_pwm_magnitude = speed_to_base_pwm(target_speed);
        base_pwm = base_pwm_magnitude;
    } else {
        base_pwm = 0;
    }

    p_step = (error * kp_milli) / 1000;
    d_step = (normalized_d_error * kd_milli) / 1000;
    requested_signed_pwm = base_pwm + p_step + d_step;
    requested_signed_pwm = clamp_int(requested_signed_pwm,
                                     -CONVEYOR_SPEED_PID_PWM_MAX,
                                     CONVEYOR_SPEED_PID_PWM_MAX);
    slew_signed_pwm_toward(&current_signed_pwm, requested_signed_pwm);
    current_signed_pwm = clamp_int(current_signed_pwm,
                                   -CONVEYOR_SPEED_PID_PWM_MAX,
                                   CONVEYOR_SPEED_PID_PWM_MAX);

    if (current_signed_pwm < 0) {
        *direction = 0;
        *pwm = -current_signed_pwm;
    } else {
        *direction = 1;
        *pwm = current_signed_pwm;
    }

    if (debug != NULL) {
        debug->target_speed = target_speed;
        debug->speed = speed;
        debug->error = error;
        debug->base_pwm = base_pwm;
        debug->p_step = p_step;
        debug->d_step = d_step;
        debug->requested_pwm = requested_signed_pwm;
        debug->applied_pwm = *pwm;
        debug->direction = *direction;
        debug->kp_milli = kp_milli;
        debug->kd_milli = kd_milli;
    }

    xSemaphoreTake(motor_mutex, portMAX_DELAY);
    motor->pwm = *pwm;
    motor->direction = *direction;
    xSemaphoreGive(motor_mutex);
}

static void apply_motor_output(motor_t *motor, int direction, int pwm)
{
    int rpwm = 0;
    int lpwm = 0;

    if (direction == 1) {
        rpwm = pwm;
    } else {
        lpwm = pwm;
    }

    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, motor->rpwm_ledc_channel, rpwm));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, motor->rpwm_ledc_channel));
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, motor->lpwm_ledc_channel, lpwm));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, motor->lpwm_ledc_channel));
}

void motor_pid_task(void *arg)
{
    motor_t *motor = (motor_t *)arg;
    int count = 0;
    int last_count = 0;
    int raw_speed = 0;
    int speed = 0;
    int speed_samples[SPEED_AVG_SAMPLE_COUNT] = {0};
    int speed_sample_index = 0;
    int speed_sample_count = 0;
    int speed_sample_sum = 0;
    int last_error = 0;
    int pwm = 0;
    int direction = 0;
    pid_debug_t debug = {0};
    int watch_ticks = 0;
    int watch_period_ticks = ENCODER_WATCH_DELAY_MS / MOTOR_PID_DELAY_MS;

    if (watch_period_ticks < 1) {
        watch_period_ticks = 1;
    }

    ESP_ERROR_CHECK(pcnt_unit_get_count(motor->pcnt_unit, &last_count));

    while (1) {
        ESP_ERROR_CHECK(pcnt_unit_get_count(motor->pcnt_unit, &count));
        raw_speed = (((count - last_count) * 1000) / MOTOR_PID_DELAY_MS) * CONVEYOR_ENCODER_SPEED_SIGN;
        last_count = count;

        speed_sample_sum -= speed_samples[speed_sample_index];
        speed_samples[speed_sample_index] = raw_speed;
        speed_sample_sum += raw_speed;
        if (speed_sample_count < SPEED_AVG_SAMPLE_COUNT) {
            speed_sample_count++;
        }
        speed_sample_index++;
        if (speed_sample_index >= SPEED_AVG_SAMPLE_COUNT) {
            speed_sample_index = 0;
        }
        speed = speed_sample_sum / speed_sample_count;

        xSemaphoreTake(motor_mutex, portMAX_DELAY);
        motor->position = count;
        motor->current_speed = speed;
        xSemaphoreGive(motor_mutex);

        update_motor_speed_control(motor, speed, &last_error, &direction, &pwm, &debug);
        apply_motor_output(motor, direction, pwm);

        watch_ticks++;
        if (watch_ticks >= watch_period_ticks) {
            watch_ticks = 0;

            if (encoder_watch_enabled && encoder_watch_motor == motor) {
                console_printf("EVENT ENCODER %s %d %d\r\n", motor->name, count, speed);
            }

            if (pid_watch_enabled && pid_watch_motor == motor) {
                console_printf("EVENT PID %s target=%d speed=%d error=%d base=%d p=%d d=%d requested=%d pwm=%d dir=%d kp=%d kd=%d\r\n",
                               motor->name,
                               debug.target_speed,
                               debug.speed,
                               debug.error,
                               debug.base_pwm,
                               debug.p_step,
                               debug.d_step,
                               debug.requested_pwm,
                               debug.applied_pwm,
                               debug.direction,
                               debug.kp_milli,
                               debug.kd_milli);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(MOTOR_PID_DELAY_MS));
    }
}
