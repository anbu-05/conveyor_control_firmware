#include "app_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "microrl.h"

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
            console_print("ERR BAD_ARGS\r\n");
            return 0;
        }

        motor = find_motor(argv[1]);
        if (motor == NULL) {
            console_print("ERR UNKNOWN_MOTOR\r\n");
            return 0;
        }

        if (argv[2][0] == '\0') {
            console_print("ERR BAD_PWM\r\n");
            return 0;
        }

        for (int i = 0; argv[2][i] != '\0'; i++) {
            if (argv[2][i] < '0' || argv[2][i] > '9') {
                console_print("ERR BAD_PWM\r\n");
                return 0;
            }
        }

        if (argv[3][0] == '\0') {
            console_print("ERR BAD_DIRECTION\r\n");
            return 0;
        }

        for (int i = 0; argv[3][i] != '\0'; i++) {
            if (argv[3][i] < '0' || argv[3][i] > '9') {
                console_print("ERR BAD_DIRECTION\r\n");
                return 0;
            }
        }

        pwm = strtol(argv[2], NULL, 10);
        direction = strtol(argv[3], NULL, 10);

        if (pwm < 0 || pwm > MOTOR_PWM_MAX) {
            console_print("ERR BAD_PWM\r\n");
            return 0;
        }

        if (direction != 0 && direction != 1) {
            console_print("ERR BAD_DIRECTION\r\n");
            return 0;
        }

        xSemaphoreTake(motor_mutex, portMAX_DELAY);
        motor->pwm = (int)pwm;
        motor->direction = (int)direction;
        xSemaphoreGive(motor_mutex);

        console_printf("OK SETMOTOR %s\r\n", motor->name);
        return 0;
    }

    if (strcmp(argv[0], "stopmotor") == 0) {
        if (argc != 2) {
            console_print("ERR BAD_ARGS\r\n");
            return 0;
        }

        motor = find_motor(argv[1]);
        if (motor == NULL) {
            console_print("ERR UNKNOWN_MOTOR\r\n");
            return 0;
        }

        xSemaphoreTake(motor_mutex, portMAX_DELAY);
        motor->pwm = 0;
        xSemaphoreGive(motor_mutex);

        console_printf("OK STOPMOTOR %s\r\n", motor->name);
        return 0;
    }

    if (strcmp(argv[0], "stop") == 0) {
        if (argc != 1) {
            console_print("ERR BAD_ARGS\r\n");
            return 0;
        }

        xSemaphoreTake(motor_mutex, portMAX_DELAY);
        for (int i = 0; i < MOTOR_COUNT; i++) {
            motors[i].pwm = 0;
        }
        xSemaphoreGive(motor_mutex);

        console_print("OK STOP\r\n");
        return 0;
    }

    if (strcmp(argv[0], "watchsensors") == 0) {
        if (argc != 2) {
            console_print("ERR BAD_ARGS\r\n");
            return 0;
        }

        if (strcmp(argv[1], "on") == 0) {
            sensor_watch_enabled = true;
            console_print("OK WATCHSENSORS ON\r\n");
            return 0;
        }

        if (strcmp(argv[1], "off") == 0) {
            sensor_watch_enabled = false;
            console_print("OK WATCHSENSORS OFF\r\n");
            return 0;
        }

        console_print("ERR BAD_ARGS\r\n");
        return 0;
    }

    console_print("ERR UNKNOWN_COMMAND\r\n");
    return 0;
}

/*
 * FreeRTOS task for the serial command layer.
 * It reads bytes from stdin, feeds them into microrl, and lets microrl call
 * execute_command when a full line is entered.
 */
void microrl_task(void *arg)
{
    microrl_t rl;
    int ch = 0;

    (void)arg;

    microrl_init(&rl, console_print);
    microrl_set_execute_callback(&rl, execute_command);

    /* This task only parses commands and updates shared state. */
    while (1) {
        ch = getchar();

        if (ch != EOF) {
            microrl_insert_char(&rl, ch);
        } else {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}
