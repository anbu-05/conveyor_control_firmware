#ifndef APP_STATE_H
#define APP_STATE_H

#include <stdbool.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define MOTOR_COUNT 1
#define MOTOR_PWM_FREQ_HZ 20000
#define MOTOR_PWM_RESOLUTION LEDC_TIMER_8_BIT
#define MOTOR_PWM_MAX 255
#define MOTOR_CONTROLLER_DELAY_MS 20
#define MICRORL_TASK_STACK_SIZE 4096
#define MOTOR_TASK_STACK_SIZE 3072
#define SENSOR_COUNT 2
#define SENSOR_TASK_STACK_SIZE 3072
#define SENSOR_POLL_DELAY_MS 20

typedef struct {
    const char *name;
    int pwm;
    int direction;
    int position;
    int target_pos;
    bool pos_control;
    gpio_num_t pwm_gpio;
    gpio_num_t dir_gpio;
    ledc_channel_t ledc_channel;
} motor_t;

typedef struct {
    const char *name;
    gpio_num_t gpio;
    int value;
    int last_value;
} sensor_t;

extern SemaphoreHandle_t motor_mutex;
extern SemaphoreHandle_t console_mutex;
extern volatile bool sensor_watch_enabled;
extern motor_t motors[MOTOR_COUNT];
extern sensor_t sensors[SENSOR_COUNT];

void console_print(const char *text);
void console_printf(const char *format, ...);
motor_t *find_motor(const char *name);
void move_main_motor(int direction, int pwm);
void stop_all_motors(void);

void configure_pwm(void);
void configure_sensors(void);
void microrl_task(void *arg);
void motor_controller_task(void *arg);
void sensor_reader_task(void *arg);

#endif
