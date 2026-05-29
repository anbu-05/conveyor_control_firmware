#include "app_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "conveyor_job.h"
#include "microrl.h"

static bool parse_job_direction(const char *text, conveyor_direction_t *direction)
{
    if (strcmp(text, "left") == 0) {
        *direction = CONVEYOR_DIR_LEFT;
        return true;
    }

    if (strcmp(text, "right") == 0) {
        *direction = CONVEYOR_DIR_RIGHT;
        return true;
    }

    return false;
}

static void send_job_command(conveyor_cmd_t command, const char *ok_text)
{
    if ((command.type == CONVEYOR_CMD_START_TX || command.type == CONVEYOR_CMD_START_RX) &&
        !conveyor_job_is_idle()) {
        console_print("ERR JOB_BUSY\r\n");
        return;
    }

    if (!conveyor_job_send_command(command)) {
        console_print("ERR JOB_QUEUE\r\n");
        return;
    }

    console_print(ok_text);
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
        conveyor_cmd_t command = {
            .type = CONVEYOR_CMD_EMERGENCY_STOP,
            .direction = CONVEYOR_DIR_RIGHT,
        };

        if (argc != 1) {
            console_print("ERR BAD_ARGS\r\n");
            return 0;
        }

        stop_all_motors();
        (void)conveyor_job_send_command(command);

        console_print("OK STOP\r\n");
        return 0;
    }

    if (strcmp(argv[0], "jobtx") == 0) {
        conveyor_cmd_t command;

        if (argc != 2) {
            console_print("ERR BAD_ARGS\r\n");
            return 0;
        }

        if (!parse_job_direction(argv[1], &command.direction)) {
            console_print("ERR BAD_DIRECTION\r\n");
            return 0;
        }

        command.type = CONVEYOR_CMD_START_TX;
        send_job_command(command, "OK JOBTX\r\n");
        return 0;
    }

    if (strcmp(argv[0], "jobrx") == 0) {
        conveyor_cmd_t command;

        if (argc != 2) {
            console_print("ERR BAD_ARGS\r\n");
            return 0;
        }

        if (!parse_job_direction(argv[1], &command.direction)) {
            console_print("ERR BAD_DIRECTION\r\n");
            return 0;
        }

        command.type = CONVEYOR_CMD_START_RX;
        send_job_command(command, "OK JOBRX\r\n");
        return 0;
    }

    if (strcmp(argv[0], "estop") == 0) {
        conveyor_cmd_t command = {
            .type = CONVEYOR_CMD_EMERGENCY_STOP,
            .direction = CONVEYOR_DIR_RIGHT,
        };

        if (argc != 1) {
            console_print("ERR BAD_ARGS\r\n");
            return 0;
        }

        send_job_command(command, "OK ESTOP\r\n");
        return 0;
    }

    if (strcmp(argv[0], "clearerror") == 0) {
        conveyor_cmd_t command = {
            .type = CONVEYOR_CMD_CLEAR_ERROR,
            .direction = CONVEYOR_DIR_RIGHT,
        };

        if (argc != 1) {
            console_print("ERR BAD_ARGS\r\n");
            return 0;
        }

        send_job_command(command, "OK CLEARERROR\r\n");
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
