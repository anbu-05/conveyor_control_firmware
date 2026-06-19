#include "app_state.h"

#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "esp_wifi.h"
#include "runtime_config.h"

SemaphoreHandle_t motor_mutex;
SemaphoreHandle_t console_mutex;
volatile bool sensor_watch_enabled = false;
volatile bool encoder_watch_enabled = false;
volatile bool pid_watch_enabled = false;
motor_t *encoder_watch_motor = NULL;
motor_t *pid_watch_motor = NULL;

/* Keep all motor state and pin config together so more motors can be added later. */
motor_t motors[MOTOR_COUNT] = {
    {
        .name = "M0",
        .pwm = 0,
        .direction = 0,
        .position = 0,
        .target_pos = 0,
        .target_speed = 0,
        .current_speed = 0,
        .pos_control = false,
        .speed_control = false,
        .rpwm_gpio = MOTOR_RPWM_GPIO,
        .lpwm_gpio = MOTOR_LPWM_GPIO,
        .ren_gpio = MOTOR_REN_GPIO,
        .len_gpio = MOTOR_LEN_GPIO,
        .encoder_a_gpio = GPIO_NUM_17,
        .encoder_b_gpio = GPIO_NUM_18,
        .rpwm_ledc_channel = LEDC_CHANNEL_0,
        .lpwm_ledc_channel = LEDC_CHANNEL_1,
        .pcnt_unit = NULL,
    },
};

static conveyor_travel_direction_t travel_direction = CONVEYOR_TRAVEL_S0_TO_S1;

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

bool move_motor(const char *name, int direction, int pwm)
{
    motor_t *motor = find_motor(name);

    if (motor == NULL) {
        return false;
    }

    if (pwm < 0) {
        pwm = 0;
    }
    if (pwm > MOTOR_PWM_MAX) {
        pwm = MOTOR_PWM_MAX;
    }

    xSemaphoreTake(motor_mutex, portMAX_DELAY);
    motor->direction = direction;
    motor->pwm = pwm;
    motor->target_speed = 0;
    motor->speed_control = false;
    xSemaphoreGive(motor_mutex);
    return true;
}

bool start_motor(const char *name)
{
    motor_t *motor = find_motor(name);
    int speed = runtime_config_run_speed_counts_per_sec();
    int base_dir = CONVEYOR_MOTOR_FORWARD_DIRECTION;
    conveyor_travel_direction_t dir = conveyor_get_travel_direction();

    if (motor == NULL) {
        return false;
    }

    if (speed < 0) {
        speed = -speed;
    }

    if (dir == CONVEYOR_TRAVEL_S1_TO_S0) {
        base_dir = (base_dir == 0) ? 1 : 0;
    }

    xSemaphoreTake(motor_mutex, portMAX_DELAY);
    if (base_dir == 0) {
        motor->target_speed = -speed;
    } else {
        motor->target_speed = speed;
    }
    motor->speed_control = true;
    xSemaphoreGive(motor_mutex);
    return true;
}

bool stop_motor(const char *name)
{
    motor_t *motor = find_motor(name);

    if (motor == NULL) {
        return false;
    }

    xSemaphoreTake(motor_mutex, portMAX_DELAY);
    motor->target_speed = 0;
    motor->speed_control = true;
    xSemaphoreGive(motor_mutex);
    return true;
}

void stop_all_motors(void)
{
    xSemaphoreTake(motor_mutex, portMAX_DELAY);
    for (int i = 0; i < MOTOR_COUNT; i++) {
        motors[i].pwm = 0;
        motors[i].target_speed = 0;
        motors[i].speed_control = false;
    }
    xSemaphoreGive(motor_mutex);
}

conveyor_travel_direction_t conveyor_get_travel_direction(void)
{
    return travel_direction;
}

void conveyor_set_travel_direction(conveyor_travel_direction_t dir)
{
    travel_direction = dir;
}

const char *conveyor_travel_direction_name(conveyor_travel_direction_t dir)
{
    if (dir == CONVEYOR_TRAVEL_S0_TO_S1) {
        return "S0_TO_S1";
    }
    return "S1_TO_S0";
}

int conveyor_get_rssi(void)
{
    wifi_ap_record_t ap_info;
    esp_err_t err = esp_wifi_sta_get_ap_info(&ap_info);
    if (err != ESP_OK) {
        return INT16_MIN;
    }
    return ap_info.rssi;
}
