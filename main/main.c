#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "microrl.h"

#define MOTOR_COUNT 1
#define MOTOR_PWM_FREQ_HZ 20000
#define MOTOR_PWM_RESOLUTION LEDC_TIMER_8_BIT
#define MOTOR_PWM_MAX 255
#define MOTOR_CONTROLLER_DELAY_MS 20
#define MICRORL_TASK_STACK_SIZE 4096
#define MOTOR_TASK_STACK_SIZE 3072

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

static const char *TAG = "conveyor";
static SemaphoreHandle_t motor_mutex;

/* Keep all motor state and pin config together so more motors can be added later. */
static motor_t motors[MOTOR_COUNT] = {
    {
        .name = "M0",
        .pwm = 0,
        .direction = 0,
        .position = 0,
        .target_pos = 0,
        .pos_control = false,
        .pwm_gpio = GPIO_NUM_7,
        .dir_gpio = GPIO_NUM_6,
        .ledc_channel = LEDC_CHANNEL_0,
    },
};

/*
 * Prints text back to the serial monitor.
 * Microrl uses this as its output callback, and the command handlers use it
 * for short status and error messages.
 */
static void console_print(const char *text)
{
    printf("%s", text);
    fflush(stdout);
}

/*
 * Finds the motor whose struct name matches the command argument.
 * This keeps command handling tied to the motor table instead of hardcoding
 * motors[0] everywhere.
 */
static motor_t *find_motor(const char *name)
{
    for (int i = 0; i < MOTOR_COUNT; i++) {
        if (strcmp(motors[i].name, name) == 0) {
            return &motors[i];
        }
    }

    return NULL;
}

/*
 * Dispatches a parsed microrl command to the matching command handler.
 * Command names are compared literally, so case changes or aliases are
 * rejected.
 */
static int execute_command(int argc, const char *const *argv)
{
    long pwm = 0;
    long direction = 0;
    motor_t *motor = NULL;

    if (strcmp(argv[0], "setmotor") == 0) {
        if (argc != 4) {
            console_print("usage: setmotor M0 128 1\r\n");
            return 0;
        }

        motor = find_motor(argv[1]);
        if (motor == NULL) {
            console_print("unknown motor\r\n");
            return 0;
        }

        if (argv[2][0] == '\0') {
            console_print("pwm must be 0 to 255\r\n");
            return 0;
        }

        for (int i = 0; argv[2][i] != '\0'; i++) {
            if (argv[2][i] < '0' || argv[2][i] > '9') {
                console_print("pwm must be 0 to 255\r\n");
                return 0;
            }
        }

        if (argv[3][0] == '\0') {
            console_print("direction must be 0 or 1\r\n");
            return 0;
        }

        for (int i = 0; argv[3][i] != '\0'; i++) {
            if (argv[3][i] < '0' || argv[3][i] > '9') {
                console_print("direction must be 0 or 1\r\n");
                return 0;
            }
        }

        pwm = strtol(argv[2], NULL, 10);
        direction = strtol(argv[3], NULL, 10);

        if (pwm < 0 || pwm > MOTOR_PWM_MAX) {
            console_print("pwm must be 0 to 255\r\n");
            return 0;
        }

        if (direction != 0 && direction != 1) {
            console_print("direction must be 0 or 1\r\n");
            return 0;
        }

        xSemaphoreTake(motor_mutex, portMAX_DELAY);
        motor->pwm = (int)pwm;
        motor->direction = (int)direction;
        xSemaphoreGive(motor_mutex);

        console_print("ok\r\n");
        return 0;
    }

    if (strcmp(argv[0], "stopmotor") == 0) {
        if (argc != 2) {
            console_print("usage: stopmotor M0\r\n");
            return 0;
        }

        motor = find_motor(argv[1]);
        if (motor == NULL) {
            console_print("unknown motor\r\n");
            return 0;
        }

        xSemaphoreTake(motor_mutex, portMAX_DELAY);
        motor->pwm = 0;
        xSemaphoreGive(motor_mutex);

        console_print("ok\r\n");
        return 0;
    }

    if (strcmp(argv[0], "stop") == 0) {
        if (argc != 1) {
            console_print("usage: stop\r\n");
            return 0;
        }

        xSemaphoreTake(motor_mutex, portMAX_DELAY);
        for (int i = 0; i < MOTOR_COUNT; i++) {
            motors[i].pwm = 0;
        }
        xSemaphoreGive(motor_mutex);

        console_print("ok\r\n");
        return 0;
    }

    console_print("unknown command\r\n");
    return 0;
}

/*
 * Configures standard input and output for command text.
 * ESP-IDF decides whether stdin/stdout use USB Serial/JTAG or UART based on
 * sdkconfig.
 */
static void configure_console(void)
{
    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
}

/*
 * Configures the shared LEDC timer and then attaches every motor's PWM pin to
 * its channel.
 */
static void configure_pwm(void)
{
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = MOTOR_PWM_RESOLUTION,
        .freq_hz = MOTOR_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };

    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    for (int i = 0; i < MOTOR_COUNT; i++) {
        ledc_channel_config_t ledc_channel = {
            .gpio_num = motors[i].pwm_gpio,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = motors[i].ledc_channel,
            .timer_sel = LEDC_TIMER_0,
            .duty = 0,
            .hpoint = 0,
        };

        ESP_ERROR_CHECK(gpio_set_direction(motors[i].dir_gpio, GPIO_MODE_OUTPUT));
        ESP_ERROR_CHECK(gpio_set_level(motors[i].dir_gpio, 0));
        ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
    }
}

/*
 * FreeRTOS task for the serial command layer.
 * It reads bytes from stdin, feeds them into microrl, and lets microrl call
 * execute_command when a full line is entered.
 */
static void microrl_task(void *arg)
{
    microrl_t rl;
    int ch = 0;
    int last_char_was_cr = 0;

    (void)arg;

    microrl_init(&rl, console_print);
    microrl_set_execute_callback(&rl, execute_command);
    console_print("\r\nconveyor ready\r\n> ");

    /* This task only parses commands and updates the motor struct. */
    while (1) {
        ch = getchar();

        if (ch != EOF) {
            int print_prompt = (ch == '\r' || (ch == '\n' && last_char_was_cr == 0));

            microrl_insert_char(&rl, ch);

            if (print_prompt) {
                console_print("> ");
            }

            last_char_was_cr = (ch == '\r') ? 1 : 0;
        } else {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

/*
 * FreeRTOS task for one motor output loop.
 * It copies the latest PWM and direction from the motor struct, then writes
 * those values to GPIO and LEDC hardware.
 */
static void motor_controller_task(void *arg)
{
    motor_t *motor = (motor_t *)arg;
    int pwm = 0;
    int direction = 0;

    /* This task is the only place that writes motor state to GPIO and PWM hardware. */
    while (1) {
        xSemaphoreTake(motor_mutex, portMAX_DELAY);
        pwm = motor->pwm;
        direction = motor->direction;
        xSemaphoreGive(motor_mutex);

        ESP_ERROR_CHECK(gpio_set_level(motor->dir_gpio, direction));
        ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, motor->ledc_channel, pwm));
        ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, motor->ledc_channel));

        vTaskDelay(pdMS_TO_TICKS(MOTOR_CONTROLLER_DELAY_MS));
    }
}

/*
 * ESP-IDF application entry point.
 * It creates shared state protection, configures hardware, and starts the two
 * tasks used by this first version of the project.
 */
void app_main(void)
{
    motor_mutex = xSemaphoreCreateMutex();
    if (motor_mutex == NULL) {
        ESP_LOGE(TAG, "failed to create motor mutex");
        return;
    }

    configure_console();
    configure_pwm();

    xTaskCreate(microrl_task, "microrl", MICRORL_TASK_STACK_SIZE, NULL, 5, NULL);
    xTaskCreate(motor_controller_task, "motor_ctrl_M0", MOTOR_TASK_STACK_SIZE, &motors[0], 5, NULL);
}
