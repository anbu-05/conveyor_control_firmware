#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/uart.h"
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
#define UART_READ_TIMEOUT_MS 20

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

static void console_print(const char *text)
{
    printf("%s", text);
    fflush(stdout);
}

static int parse_int_strict(const char *text, int *value)
{
    char *end = NULL;
    long result = strtol(text, &end, 10);

    if (text[0] == '\0') {
        return 0;
    }

    for (int i = 0; text[i] != '\0'; i++) {
        if (text[i] < '0' || text[i] > '9') {
            return 0;
        }
    }

    if (end == NULL || end[0] != '\0') {
        return 0;
    }

    *value = (int)result;
    return 1;
}

static motor_t *find_motor(const char *name)
{
    for (int i = 0; i < MOTOR_COUNT; i++) {
        if (strcmp(motors[i].name, name) == 0) {
            return &motors[i];
        }
    }

    return NULL;
}

static void set_motor_pwm(motor_t *motor, int pwm)
{
    xSemaphoreTake(motor_mutex, portMAX_DELAY);
    motor->pwm = pwm;
    xSemaphoreGive(motor_mutex);
}

static void set_all_motor_pwm(int pwm)
{
    xSemaphoreTake(motor_mutex, portMAX_DELAY);
    for (int i = 0; i < MOTOR_COUNT; i++) {
        motors[i].pwm = pwm;
    }
    xSemaphoreGive(motor_mutex);
}

static void set_motor_pwm_direction(motor_t *motor, int pwm, int direction)
{
    xSemaphoreTake(motor_mutex, portMAX_DELAY);
    motor->pwm = pwm;
    motor->direction = direction;
    xSemaphoreGive(motor_mutex);
}

static int command_setmotor(int argc, const char *const *argv)
{
    int pwm = 0;
    int direction = 0;
    motor_t *motor = NULL;

    if (argc != 4) {
        console_print("usage: setmotor M0 128 1\r\n");
        return 0;
    }

    motor = find_motor(argv[1]);
    if (motor == NULL) {
        console_print("unknown motor\r\n");
        return 0;
    }

    if (parse_int_strict(argv[2], &pwm) == 0 || pwm < 0 || pwm > MOTOR_PWM_MAX) {
        console_print("pwm must be 0 to 255\r\n");
        return 0;
    }

    if (parse_int_strict(argv[3], &direction) == 0 || (direction != 0 && direction != 1)) {
        console_print("direction must be 0 or 1\r\n");
        return 0;
    }

    set_motor_pwm_direction(motor, pwm, direction);
    console_print("ok\r\n");
    return 0;
}

static int command_stopmotor(int argc, const char *const *argv)
{
    motor_t *motor = NULL;

    if (argc != 2) {
        console_print("usage: stopmotor M0\r\n");
        return 0;
    }

    motor = find_motor(argv[1]);
    if (motor == NULL) {
        console_print("unknown motor\r\n");
        return 0;
    }

    set_motor_pwm(motor, 0);
    console_print("ok\r\n");
    return 0;
}

static int command_stop(int argc, const char *const *argv)
{
    (void)argv;

    if (argc != 1) {
        console_print("usage: stop\r\n");
        return 0;
    }

    set_all_motor_pwm(0);
    console_print("ok\r\n");
    return 0;
}

static int execute_command(int argc, const char *const *argv)
{
    if (strcmp(argv[0], "setmotor") == 0) {
        return command_setmotor(argc, argv);
    }

    if (strcmp(argv[0], "stopmotor") == 0) {
        return command_stopmotor(argc, argv);
    }

    if (strcmp(argv[0], "stop") == 0) {
        return command_stop(argc, argv);
    }

    console_print("unknown command\r\n");
    return 0;
}

static void configure_uart(void)
{
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 1024, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_0, &uart_config));
}

static void configure_motor_output(const motor_t *motor)
{
    ledc_channel_config_t ledc_channel = {
        .gpio_num = motor->pwm_gpio,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = motor->ledc_channel,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };

    ESP_ERROR_CHECK(gpio_set_direction(motor->dir_gpio, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_level(motor->dir_gpio, 0));
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
}

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
        configure_motor_output(&motors[i]);
    }
}

static void microrl_task(void *arg)
{
    microrl_t rl;
    uint8_t ch = 0;
    int last_char_was_cr = 0;

    (void)arg;

    microrl_init(&rl, console_print);
    microrl_set_execute_callback(&rl, execute_command);
    console_print("\r\nconveyor ready\r\n> ");

    while (1) {
        int length = uart_read_bytes(UART_NUM_0, &ch, 1, pdMS_TO_TICKS(UART_READ_TIMEOUT_MS));

        if (length > 0) {
            int print_prompt = (ch == '\r' || (ch == '\n' && last_char_was_cr == 0));

            microrl_insert_char(&rl, ch);

            if (print_prompt) {
                console_print("> ");
            }

            last_char_was_cr = (ch == '\r') ? 1 : 0;
        }
    }
}

static void motor_controller_task(void *arg)
{
    motor_t *motor = (motor_t *)arg;
    int pwm = 0;
    int direction = 0;

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

void app_main(void)
{
    motor_mutex = xSemaphoreCreateMutex();
    if (motor_mutex == NULL) {
        ESP_LOGE(TAG, "failed to create motor mutex");
        return;
    }

    configure_uart();
    configure_pwm();

    xTaskCreate(microrl_task, "microrl", MICRORL_TASK_STACK_SIZE, NULL, 5, NULL);
    xTaskCreate(motor_controller_task, "motor_ctrl_M0", MOTOR_TASK_STACK_SIZE, &motors[0], 5, NULL);
}
