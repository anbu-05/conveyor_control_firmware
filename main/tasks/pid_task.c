#include "app_state.h"

#include <stdint.h>

#include "config.h"
#include "driver/pulse_cnt.h"
#include "esp_err.h"
#include "runtime_config.h"

#define SPEED_AVG_SAMPLE_COUNT 5

static int abs_int(int value)
{
    if (value < 0) {
        return -value;
    }

    return value;
}

void pid_controller_task(void *arg)
{
    motor_t *motor = (motor_t *)arg;
    int count = 0;
    int last_count = 0;
    int raw_speed = 0;
    int speed = 0;
    int speed_samples[SPEED_AVG_SAMPLE_COUNT] = {0};
    int speed_sample_index = 0;
    int speed_sample_sum = 0;
    int target_speed = 0;
    int target_abs = 0;
    int speed_along_target = 0;
    int error = 0;
    int acceleration_step = 0;
    int planned_speed = 0;
    int pwm = 0;
    int direction = 0;
    int current_direction = 0;
    int speed_control = 0;
    int watch_ticks = 0;
    int watch_period_ticks = ENCODER_WATCH_DELAY_MS / PID_CONTROLLER_DELAY_MS;

    if (watch_period_ticks < 1) {
        watch_period_ticks = 1;
    }

    ESP_ERROR_CHECK(pcnt_unit_get_count(motor->pcnt_unit, &last_count));

    while (1) {
        ESP_ERROR_CHECK(pcnt_unit_get_count(motor->pcnt_unit, &count));
        raw_speed = ((count - last_count) * 1000) / PID_CONTROLLER_DELAY_MS;
        last_count = count;

        speed_sample_sum -= speed_samples[speed_sample_index];
        speed_samples[speed_sample_index] = raw_speed;
        speed_sample_sum += raw_speed;
        speed_sample_index++;
        if (speed_sample_index >= SPEED_AVG_SAMPLE_COUNT) {
            speed_sample_index = 0;
        }
        speed = speed_sample_sum / SPEED_AVG_SAMPLE_COUNT;

        xSemaphoreTake(motor_mutex, portMAX_DELAY);
        motor->position = count;
        motor->current_speed = speed;
        target_speed = motor->target_speed;
        planned_speed = motor->planned_speed;
        current_direction = motor->direction;
        speed_control = motor->speed_control ? 1 : 0;
        xSemaphoreGive(motor_mutex);

        if (speed_control) {
            if (target_speed == 0) {
                direction = current_direction;
                if (planned_speed > 0) {
                    acceleration_step = -((planned_speed * runtime_config_speed_kp_milli()) / 1000);
                    if (acceleration_step == 0) {
                        acceleration_step = -1;
                    }
                    planned_speed += acceleration_step;
                } else {
                    planned_speed = 0;
                }
            } else {
                if (target_speed < 0) {
                    target_abs = abs_int(target_speed);
                    speed_along_target = -speed;
                    direction = 0;
                } else {
                    target_abs = target_speed;
                    speed_along_target = speed;
                    direction = 1;
                }

                error = target_abs - speed_along_target;
                acceleration_step = (error * runtime_config_speed_kp_milli()) / 1000;
                planned_speed += acceleration_step;

                if (planned_speed < 0) {
                    planned_speed = 0;
                }
                if (planned_speed > target_abs) {
                    planned_speed = target_abs;
                }
            }

            if (planned_speed < 0) {
                planned_speed = 0;
            }

            pwm = (int)(((int64_t)planned_speed * runtime_config_speed_pwm_scale_milli()) / 1000);
            if (pwm > CONVEYOR_SPEED_PID_PWM_MAX) {
                pwm = CONVEYOR_SPEED_PID_PWM_MAX;
            }

            xSemaphoreTake(motor_mutex, portMAX_DELAY);
            motor->planned_speed = planned_speed;
            motor->pwm = pwm;
            motor->direction = direction;
            xSemaphoreGive(motor_mutex);
        }

        watch_ticks++;
        if (watch_ticks >= watch_period_ticks) {
            watch_ticks = 0;

            if (encoder_watch_enabled && encoder_watch_motor == motor) {
                console_printf("EVENT ENCODER %s %d %d\r\n", motor->name, count, speed);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(PID_CONTROLLER_DELAY_MS));
    }
}
