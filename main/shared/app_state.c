#include "app_state.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "runtime_config.h"

SemaphoreHandle_t motor_mutex;
SemaphoreHandle_t console_mutex;
volatile bool sensor_watch_enabled = false;
volatile bool encoder_watch_enabled = false;
motor_t *encoder_watch_motor = NULL;

/* Keep all motor state and pin config together so more motors can be added later. */
motor_t motors[MOTOR_COUNT] = {
    {
        .name = "M0",
        .pwm = 0,
        .direction = 0,
        .position = 0,
        .target_pos = 0,
        .target_speed = 0,
        .planned_speed = 0,
        .current_speed = 0,
        .pos_control = false,
        .speed_control = false,
        .pwm_gpio = GPIO_NUM_7,
        .dir_gpio = GPIO_NUM_6,
        .encoder_a_gpio = GPIO_NUM_15,
        .encoder_b_gpio = GPIO_NUM_16,
        .ledc_channel = LEDC_CHANNEL_0,
        .pcnt_unit = NULL,
    },
};

/* Sensor state lives in a table so more sensors can be added later. */
sensor_t sensors[SENSOR_COUNT] = {
    {
        .name = "S0",
        .gpio = GPIO_NUM_4,
        .value = 1,
        .last_value = 1,
    },
    {
        .name = "S1",
        .gpio = GPIO_NUM_5,
        .value = 1,
        .last_value = 1,
    },
};

/*
 * Prints one complete machine-readable line back to the serial monitor.
 * The mutex keeps command responses and sensor events from mixing together.
 */
void console_print(const char *text)
{
    if (console_mutex != NULL) {
        xSemaphoreTake(console_mutex, portMAX_DELAY);
    }

    printf("%s", text);
    fflush(stdout);

    if (console_mutex != NULL) {
        xSemaphoreGive(console_mutex);
    }
}

/*
 * Prints a formatted machine-readable line.
 * This is used for responses or events that include names and values.
 */
void console_printf(const char *format, ...)
{
    va_list args;

    if (console_mutex != NULL) {
        xSemaphoreTake(console_mutex, portMAX_DELAY);
    }

    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    fflush(stdout);

    if (console_mutex != NULL) {
        xSemaphoreGive(console_mutex);
    }
}

/*
 * Finds the motor whose struct name matches the command argument.
 * This keeps command handling tied to the motor table instead of hardcoding
 * motors[0] everywhere.
 */
motor_t *find_motor(const char *name)
{
    for (int i = 0; i < MOTOR_COUNT; i++) {
        if (strcmp(motors[i].name, name) == 0) {
            return &motors[i];
        }
    }

    return NULL;
}

void move_main_motor(int direction, int pwm)
{
    if (pwm < 0) {
        pwm = 0;
    }
    if (pwm > MOTOR_PWM_MAX) {
        pwm = MOTOR_PWM_MAX;
    }

    xSemaphoreTake(motor_mutex, portMAX_DELAY);
    motors[0].direction = direction;
    motors[0].pwm = pwm;
    motors[0].target_speed = 0;
    motors[0].planned_speed = 0;
    motors[0].speed_control = false;
    xSemaphoreGive(motor_mutex);
}

void start_main_motor(void)
{
    int speed = runtime_config_run_speed_counts_per_sec();

    if (speed < 0) {
        speed = -speed;
    }

    xSemaphoreTake(motor_mutex, portMAX_DELAY);
    if (CONVEYOR_MOTOR_FORWARD_DIRECTION == 0) {
        if (motors[0].target_speed > 0) {
            motors[0].planned_speed = 0;
        }
        motors[0].target_speed = -speed;
    } else {
        if (motors[0].target_speed < 0) {
            motors[0].planned_speed = 0;
        }
        motors[0].target_speed = speed;
    }
    motors[0].speed_control = true;
    xSemaphoreGive(motor_mutex);
}

void stop_all_motors(void)
{
    xSemaphoreTake(motor_mutex, portMAX_DELAY);
    for (int i = 0; i < MOTOR_COUNT; i++) {
        motors[i].pwm = 0;
        motors[i].target_speed = 0;
        motors[i].planned_speed = 0;
        motors[i].speed_control = false;
    }
    xSemaphoreGive(motor_mutex);
}
