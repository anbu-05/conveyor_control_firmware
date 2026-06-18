#ifndef APP_STATE_H
#define APP_STATE_H

#include <stdbool.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/pulse_cnt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

typedef enum {
    CONVEYOR_TRAVEL_S0_TO_S1,
    CONVEYOR_TRAVEL_S1_TO_S0
} conveyor_travel_direction_t;

#define MOTOR_COUNT 1
#define MOTOR_PWM_FREQ_HZ 20000
#define MOTOR_PWM_RESOLUTION LEDC_TIMER_8_BIT
#define MOTOR_PWM_MAX 255
#define MICRORL_TASK_STACK_SIZE 4096
#define MOTOR_PID_TASK_STACK_SIZE 4096
#define MOTOR_PID_DELAY_MS 20
#define SENSOR_COUNT 2
#define SENSOR_TASK_STACK_SIZE 3072
#define SENSOR_POLL_DELAY_MS 20
#define ENCODER_WATCH_DELAY_MS 100
#define ENCODER_GLITCH_FILTER_NS 1000
#define ENCODER_PCNT_HIGH_LIMIT 32767
#define ENCODER_PCNT_LOW_LIMIT -32768

typedef struct {
    const char *name;
    int pwm;
    int direction;
    int position;
    int target_pos;
    int target_speed;
    int current_speed;
    bool pos_control;
    bool speed_control;
    gpio_num_t rpwm_gpio;
    gpio_num_t lpwm_gpio;
    gpio_num_t ren_gpio;
    gpio_num_t len_gpio;
    gpio_num_t encoder_a_gpio;
    gpio_num_t encoder_b_gpio;
    ledc_channel_t rpwm_ledc_channel;
    ledc_channel_t lpwm_ledc_channel;
    pcnt_unit_handle_t pcnt_unit;
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
extern volatile bool encoder_watch_enabled;
extern motor_t *encoder_watch_motor;
extern motor_t motors[MOTOR_COUNT];
extern sensor_t sensors[SENSOR_COUNT];

void console_print(const char *text);
void console_printf(const char *format, ...);
motor_t *find_motor(const char *name);
bool move_motor(const char *name, int direction, int pwm);
bool start_motor(const char *name);
bool stop_motor(const char *name);
void stop_all_motors(void);

void configure_pwm(void);
void configure_sensors(void);
void configure_encoders(void);
void microrl_task(void *arg);
void motor_pid_task(void *arg);
void sensor_reader_task(void *arg);

conveyor_travel_direction_t conveyor_get_travel_direction(void);
void conveyor_set_travel_direction(conveyor_travel_direction_t dir);
const char *conveyor_travel_direction_name(conveyor_travel_direction_t dir);
int conveyor_get_rssi(void);

#endif
