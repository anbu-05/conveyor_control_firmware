#include "app_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "conveyor_job.h"
#include "esp_log.h"
#include "esp_err.h"
#include "microrl.h"
#include "runtime_config.h"
#include "sd_event_logger.h"

static void log_serial_command(uint32_t command_id, const char *text, const char *result, const char *error)
{
    ESP_LOGI("SDLOG_CMD",
             "command_id=%lu source=serial topic=serial payload=%s result=%s error=%s",
             (unsigned long)command_id,
             text,
             result,
             error);
}

static void send_job_command(conveyor_cmd_t command, const char *ok_text, const char *command_text)
{
    command.command_id = sdlog_next_command_id();

    if ((command.type == CONVEYOR_CMD_START_TX || command.type == CONVEYOR_CMD_START_RX) &&
        !conveyor_job_is_idle()) {
        log_serial_command(command.command_id, command_text, "rejected", "JOB_BUSY");
        console_print("ERR JOB_BUSY\r\n");
        return;
    }

    if (command.type == CONVEYOR_CMD_START_TX && !conveyor_job_has_tray()) {
        log_serial_command(command.command_id, command_text, "rejected", "NO_TRAY");
        console_print("ERR NO_TRAY\r\n");
        return;
    }

    if (command.type == CONVEYOR_CMD_START_RX && conveyor_job_has_tray()) {
        log_serial_command(command.command_id, command_text, "rejected", "TRAY_PRESENT");
        console_print("ERR TRAY_PRESENT\r\n");
        return;
    }

    if (!conveyor_job_send_command(command)) {
        log_serial_command(command.command_id, command_text, "rejected", "JOB_QUEUE");
        console_print("ERR JOB_QUEUE\r\n");
        return;
    }

    log_serial_command(command.command_id, command_text, "accepted", "none");
    console_print(ok_text);
}

static bool parse_config_value(const char *text, int32_t *value)
{
    long parsed = 0;

    if (text == NULL || value == NULL || text[0] == '\0') {
        return false;
    }

    for (int i = 0; text[i] != '\0'; i++) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }
    }

    parsed = strtol(text, NULL, 10);
    if (parsed < 0 || parsed > 600000) {
        return false;
    }

    *value = (int32_t)parsed;
    return true;
}

static bool parse_signed_int(const char *text, int32_t *value)
{
    long parsed = 0;
    int start = 0;

    if (text == NULL || value == NULL || text[0] == '\0') {
        return false;
    }

    if (text[0] == '-') {
        start = 1;
        if (text[1] == '\0') {
            return false;
        }
    }

    for (int i = start; text[i] != '\0'; i++) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }
    }

    parsed = strtol(text, NULL, 10);
    if (parsed < -100000 || parsed > 100000) {
        return false;
    }

    *value = (int32_t)parsed;
    return true;
}

static bool parse_gain_milli(const char *text, const char *config_name, int32_t *value)
{
    int32_t whole = 0;
    int32_t fraction = 0;
    int fraction_digits = 0;
    int digit_count = 0;
    int i = 0;

    if (text == NULL || value == NULL || text[0] == '\0') {
        return false;
    }

    while (text[i] >= '0' && text[i] <= '9') {
        whole = whole * 10 + (text[i] - '0');
        digit_count++;
        i++;
    }

    if (text[i] == '.') {
        i++;
        while (text[i] >= '0' && text[i] <= '9' && fraction_digits < 3) {
            fraction = fraction * 10 + (text[i] - '0');
            fraction_digits++;
            digit_count++;
            i++;
        }
    }

    if (text[i] != '\0' || digit_count == 0) {
        return false;
    }

    while (fraction_digits < 3) {
        fraction = fraction * 10;
        fraction_digits++;
    }

    *value = whole * 1000 + fraction;
    return runtime_config_value_is_valid(config_name, *value);
}

static void print_one_config(const char *name)
{
    int32_t value = 0;

    if (strcmp(name, "speed_kp") == 0) {
        value = runtime_config_speed_kp_milli();
        console_printf("CONFIG speed_kp %ld.%03ld\r\n", (long)(value / 1000), (long)(value % 1000));
        return;
    }

    if (strcmp(name, "speed_kd") == 0) {
        value = runtime_config_speed_kd_milli();
        console_printf("CONFIG speed_kd %ld.%03ld\r\n", (long)(value / 1000), (long)(value % 1000));
        return;
    }

    if (!runtime_config_get_value(name, &value)) {
        console_print("ERR UNKNOWN_CONFIG\r\n");
        return;
    }

    console_printf("CONFIG %s %ld\r\n", name, (long)value);
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
    int32_t speed = 0;
    int32_t kp_milli = 0;
    int32_t kd_milli = 0;
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

        (void)move_motor(motor->name, (int)direction, (int)pwm);

        console_printf("OK SETMOTOR %s\r\n", motor->name);
        return 0;
    }

    if (strcmp(argv[0], "setspeed") == 0) {
        if (argc != 3) {
            console_print("ERR BAD_ARGS\r\n");
            return 0;
        }

        motor = find_motor(argv[1]);
        if (motor == NULL) {
            console_print("ERR UNKNOWN_MOTOR\r\n");
            return 0;
        }

        if (!parse_signed_int(argv[2], &speed)) {
            console_print("ERR BAD_VALUE\r\n");
            return 0;
        }

        xSemaphoreTake(motor_mutex, portMAX_DELAY);
        motor->target_speed = speed;
        motor->speed_control = true;
        xSemaphoreGive(motor_mutex);

        console_printf("OK SETSPEED %s\r\n", motor->name);
        return 0;
    }

    if (strcmp(argv[0], "setkp") == 0) {
        if (argc != 2) {
            console_print("ERR BAD_ARGS\r\n");
            return 0;
        }

        if (!parse_gain_milli(argv[1], "speed_kp_milli", &kp_milli)) {
            console_print("ERR BAD_VALUE\r\n");
            return 0;
        }

        if (!runtime_config_set_speed_kp_milli(kp_milli)) {
            console_print("ERR CONFIG_SAVE\r\n");
            return 0;
        }

        console_printf("OK SETKP %ld.%03ld\r\n", (long)(kp_milli / 1000), (long)(kp_milli % 1000));
        return 0;
    }

    if (strcmp(argv[0], "setkd") == 0) {
        if (argc != 2) {
            console_print("ERR BAD_ARGS\r\n");
            return 0;
        }

        if (!parse_gain_milli(argv[1], "speed_kd_milli", &kd_milli)) {
            console_print("ERR BAD_VALUE\r\n");
            return 0;
        }

        if (!runtime_config_set_speed_kd_milli(kd_milli)) {
            console_print("ERR CONFIG_SAVE\r\n");
            return 0;
        }

        console_printf("OK SETKD %ld.%03ld\r\n", (long)(kd_milli / 1000), (long)(kd_milli % 1000));
        return 0;
    }

    if (strcmp(argv[0], "resetk") == 0) {
        if (argc != 1) {
            console_print("ERR BAD_ARGS\r\n");
            return 0;
        }

        if (!runtime_config_reset_speed_gains()) {
            console_print("ERR CONFIG_SAVE\r\n");
            return 0;
        }

        console_print("OK RESETK\r\n");
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

        (void)move_motor(motor->name, 0, 0);

        console_printf("OK STOPMOTOR %s\r\n", motor->name);
        return 0;
    }

    if (strcmp(argv[0], "stop") == 0) {
        conveyor_cmd_t command = {
            .type = CONVEYOR_CMD_EMERGENCY_STOP,
        };

        if (argc != 1) {
            console_print("ERR BAD_ARGS\r\n");
            return 0;
        }

        command.command_id = sdlog_next_command_id();
        stop_all_motors();
        (void)conveyor_job_send_command(command);
        log_serial_command(command.command_id, "stop", "accepted", "none");

        console_print("OK STOP\r\n");
        return 0;
    }

    if (strcmp(argv[0], "jobtx") == 0) {
        conveyor_cmd_t command;

        if (argc != 1) {
            console_print("ERR BAD_ARGS\r\n");
            return 0;
        }

        command.type = CONVEYOR_CMD_START_TX;
        send_job_command(command, "OK JOBTX\r\n", "jobtx");
        return 0;
    }

    if (strcmp(argv[0], "jobrx") == 0) {
        conveyor_cmd_t command;

        if (argc != 1) {
            console_print("ERR BAD_ARGS\r\n");
            return 0;
        }

        command.type = CONVEYOR_CMD_START_RX;
        send_job_command(command, "OK JOBRX\r\n", "jobrx");
        return 0;
    }

    if (strcmp(argv[0], "estop") == 0) {
        conveyor_cmd_t command = {
            .type = CONVEYOR_CMD_EMERGENCY_STOP,
        };

        if (argc != 1) {
            console_print("ERR BAD_ARGS\r\n");
            return 0;
        }

        send_job_command(command, "OK ESTOP\r\n", "estop");
        return 0;
    }

    if (strcmp(argv[0], "clearerror") == 0) {
        conveyor_cmd_t command = {
            .type = CONVEYOR_CMD_CLEAR_ERROR,
        };

        if (argc != 1) {
            console_print("ERR BAD_ARGS\r\n");
            return 0;
        }

        send_job_command(command, "OK CLEARERROR\r\n", "clearerror");
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

    if (strcmp(argv[0], "watchencoder") == 0) {
        if (argc != 3) {
            console_print("ERR BAD_ARGS\r\n");
            return 0;
        }

        motor = find_motor(argv[1]);
        if (motor == NULL) {
            console_print("ERR UNKNOWN_MOTOR\r\n");
            return 0;
        }

        if (strcmp(argv[2], "on") == 0) {
            encoder_watch_motor = motor;
            encoder_watch_enabled = true;
            console_printf("OK WATCHENCODER %s ON\r\n", motor->name);
            return 0;
        }

        if (strcmp(argv[2], "off") == 0) {
            if (encoder_watch_motor == motor) {
                encoder_watch_enabled = false;
                encoder_watch_motor = NULL;
            }

            console_printf("OK WATCHENCODER %s OFF\r\n", motor->name);
            return 0;
        }

        console_print("ERR BAD_ARGS\r\n");
        return 0;
    }

    if (strcmp(argv[0], "getencoder") == 0) {
        int count = 0;
        int a = 0;
        int b = 0;

        if (argc != 2) {
            console_print("ERR BAD_ARGS\r\n");
            return 0;
        }

        motor = find_motor(argv[1]);
        if (motor == NULL) {
            console_print("ERR UNKNOWN_MOTOR\r\n");
            return 0;
        }

        ESP_ERROR_CHECK(pcnt_unit_get_count(motor->pcnt_unit, &count));
        a = gpio_get_level(motor->encoder_a_gpio);
        b = gpio_get_level(motor->encoder_b_gpio);

        console_printf("ENCODER %s %d %d %d\r\n", motor->name, count, a, b);
        return 0;
    }

    if (strcmp(argv[0], "getmotor") == 0) {
        int motor_pwm = 0;
        int motor_direction = 0;
        int motor_position = 0;
        int motor_target_speed = 0;
        int motor_current_speed = 0;
        int motor_speed_control = 0;

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
        motor_pwm = motor->pwm;
        motor_direction = motor->direction;
        motor_position = motor->position;
        motor_target_speed = motor->target_speed;
        motor_current_speed = motor->current_speed;
        motor_speed_control = motor->speed_control ? 1 : 0;
        xSemaphoreGive(motor_mutex);

        console_printf("MOTOR %s %d %d %d %d %d %d\r\n",
                       motor->name,
                       motor_pwm,
                       motor_direction,
                       motor_position,
                       motor_target_speed,
                       motor_current_speed,
                       motor_speed_control);
        return 0;
    }

    if (strcmp(argv[0], "gettray") == 0) {
        conveyor_tray_status_t tray_status;

        if (argc != 1) {
            console_print("ERR BAD_ARGS\r\n");
            return 0;
        }

        conveyor_job_get_tray_status(&tray_status);
        console_printf("TRAY %s %d %d %d\r\n",
                       CONVEYOR_ID,
                       tray_status.has_tray ? 1 : 0,
                       tray_status.s0,
                       tray_status.s1);
        return 0;
    }

    if (strcmp(argv[0], "getconfig") == 0) {
        if (argc == 1) {
            runtime_config_print_all();
            return 0;
        }

        if (argc == 2) {
            print_one_config(argv[1]);
            return 0;
        }

        console_print("ERR BAD_ARGS\r\n");
        return 0;
    }

    if (strcmp(argv[0], "setconfig") == 0) {
        int32_t old_value = 0;
        int32_t new_value = 0;

        if (argc != 3) {
            console_print("ERR BAD_ARGS\r\n");
            return 0;
        }

        if (!runtime_config_get_value(argv[1], &old_value)) {
            console_print("ERR UNKNOWN_CONFIG\r\n");
            return 0;
        }

        if (!parse_config_value(argv[2], &new_value)) {
            console_print("ERR BAD_VALUE\r\n");
            return 0;
        }

        if (!runtime_config_value_is_valid(argv[1], new_value)) {
            console_print("ERR BAD_VALUE\r\n");
            return 0;
        }

        if (!conveyor_job_is_idle()) {
            console_print("ERR CONFIG_BUSY\r\n");
            return 0;
        }

        if (!runtime_config_set_value(argv[1], new_value)) {
            console_print("ERR CONFIG_SAVE\r\n");
            return 0;
        }

        console_printf("OK SETCONFIG %s %ld\r\n", argv[1], (long)new_value);
        return 0;
    }

    if (strcmp(argv[0], "resetconfig") == 0) {
        if (argc != 1) {
            console_print("ERR BAD_ARGS\r\n");
            return 0;
        }

        if (!conveyor_job_is_idle()) {
            console_print("ERR CONFIG_BUSY\r\n");
            return 0;
        }

        if (!runtime_config_reset_defaults()) {
            console_print("ERR CONFIG_SAVE\r\n");
            return 0;
        }

        console_print("OK RESETCONFIG\r\n");
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
