#include "app_state.h"

#include <stdint.h>

#include "driver/pulse_cnt.h"
#include "esp_err.h"
#include "runtime_config.h"

void pid_controller_task(void *arg)
{
    motor_t *motor = (motor_t *)arg;
    int count = 0;
    int last_count = 0;
    int speed = 0;
    int target_speed = 0;
    int error = 0;
    int64_t output = 0;
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
        speed = ((count - last_count) * 1000) / PID_CONTROLLER_DELAY_MS;
        last_count = count;

        xSemaphoreTake(motor_mutex, portMAX_DELAY);
        motor->position = count;
        motor->current_speed = speed;
        target_speed = motor->target_speed;
        current_direction = motor->direction;
        speed_control = motor->speed_control ? 1 : 0;
        xSemaphoreGive(motor_mutex);

        if (speed_control) {
            if (target_speed == 0) {
                pwm = 0;
                direction = current_direction;
            } else {
                error = target_speed - speed;
                output = ((int64_t)error * runtime_config_speed_kp_milli()) / 1000;
                if (output < 0) {
                    pwm = (int)-output;
                } else {
                    pwm = (int)output;
                }
                if (pwm > MOTOR_PWM_MAX) {
                    pwm = MOTOR_PWM_MAX;
                }
                direction = output >= 0 ? 1 : 0;
            }

            xSemaphoreTake(motor_mutex, portMAX_DELAY);
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
