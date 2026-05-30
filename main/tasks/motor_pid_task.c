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

static void slew_pwm_toward(int *pwm, int requested_pwm)
{
    if (*pwm < requested_pwm) {
        *pwm += CONVEYOR_PWM_SLEW_STEP;
        if (*pwm > requested_pwm) {
            *pwm = requested_pwm;
        }
    } else if (*pwm > requested_pwm) {
        *pwm -= CONVEYOR_PWM_SLEW_STEP;
        if (*pwm < requested_pwm) {
            *pwm = requested_pwm;
        }
    }
}

static void update_motor_speed_control(motor_t *motor, int speed, int *last_error, int *direction, int *pwm)
{
    int target_speed = 0;
    int target_abs = 0;
    int speed_along_target = 0;
    int error = 0;
    int d_error = 0;
    int normalized_d_error = 0;
    int base_pwm = 0;
    int p_step = 0;
    int d_step = 0;
    int requested_pwm = 0;
    int current_direction = 0;
    int wanted_direction = 0;
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
        return;
    }

    if (target_speed == 0) {
        *last_error = 0;
        requested_pwm = 0;
    } else {
        if (target_speed < 0) {
            target_abs = abs_int(target_speed);
            wanted_direction = 0;
        } else {
            target_abs = target_speed;
            wanted_direction = 1;
        }

        if (wanted_direction != current_direction && *pwm > 0) {
            *last_error = 0;
            requested_pwm = 0;
        } else {
            *direction = wanted_direction;
            if (wanted_direction == 0) {
                speed_along_target = -speed;
            } else {
                speed_along_target = speed;
            }

            error = target_abs - speed_along_target;
            d_error = error - *last_error;
            normalized_d_error = (d_error * SPEED_D_BASE_DELAY_MS) / MOTOR_PID_DELAY_MS;
            *last_error = error;
            base_pwm = speed_to_base_pwm(target_abs);
            p_step = (error * runtime_config_speed_kp_milli()) / 1000;
            d_step = (normalized_d_error * runtime_config_speed_kd_milli()) / 1000;
            requested_pwm = base_pwm + p_step + d_step;
        }
    }

    requested_pwm = clamp_int(requested_pwm, 0, CONVEYOR_SPEED_PID_PWM_MAX);
    slew_pwm_toward(pwm, requested_pwm);
    *pwm = clamp_int(*pwm, 0, CONVEYOR_SPEED_PID_PWM_MAX);

    xSemaphoreTake(motor_mutex, portMAX_DELAY);
    motor->pwm = *pwm;
    motor->direction = *direction;
    xSemaphoreGive(motor_mutex);
}

static void apply_motor_output(motor_t *motor, int direction, int pwm)
{
    ESP_ERROR_CHECK(gpio_set_level(motor->dir_gpio, direction));
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, motor->ledc_channel, pwm));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, motor->ledc_channel));
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
    int watch_ticks = 0;
    int watch_period_ticks = ENCODER_WATCH_DELAY_MS / MOTOR_PID_DELAY_MS;

    if (watch_period_ticks < 1) {
        watch_period_ticks = 1;
    }

    ESP_ERROR_CHECK(pcnt_unit_get_count(motor->pcnt_unit, &last_count));

    while (1) {
        ESP_ERROR_CHECK(pcnt_unit_get_count(motor->pcnt_unit, &count));
        raw_speed = ((count - last_count) * 1000) / MOTOR_PID_DELAY_MS;
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

        update_motor_speed_control(motor, speed, &last_error, &direction, &pwm);
        apply_motor_output(motor, direction, pwm);

        watch_ticks++;
        if (watch_ticks >= watch_period_ticks) {
            watch_ticks = 0;

            if (encoder_watch_enabled && encoder_watch_motor == motor) {
                console_printf("EVENT ENCODER %s %d %d\r\n", motor->name, count, speed);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(MOTOR_PID_DELAY_MS));
    }
}
