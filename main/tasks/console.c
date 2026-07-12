/*
 * ESP console task.
 * This task registers commissioning commands and forwards each command to public
 * hardware/PID/config APIs instead of owning motor behavior directly.
 */

#include "tasks/console.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config/config.h"
#include "config/runtime_config.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "linenoise/linenoise.h"
#include "sdkconfig.h"
#include "statemachine/statemachine.h"
#include "tasks/hardware.h"
#include "tasks/pid.h"

#define CONSOLE_MAX_ARGS 8
#define CONSOLE_MAX_LINE_LENGTH 256

typedef enum {
    CONSOLE_COMMAND_SETMOTOR,
    CONSOLE_COMMAND_JOBRX,
    CONSOLE_COMMAND_JOBTX,
    CONSOLE_COMMAND_GET_SMSTATUS,
    CONSOLE_COMMAND_STOP,
    CONSOLE_COMMAND_STOPMOTOR,
    CONSOLE_COMMAND_SETPOSITION,
    CONSOLE_COMMAND_GETPOSITION,
    CONSOLE_COMMAND_PID_CONTROL,
    CONSOLE_COMMAND_SETSPEED,
    CONSOLE_COMMAND_GETSPEED,
    CONSOLE_COMMAND_GET_PIDMODE,
    CONSOLE_COMMAND_SET_PIDMODE,
    CONSOLE_COMMAND_SETPID,
    CONSOLE_COMMAND_GETPID,
    CONSOLE_COMMAND_SETOFFSET,
    CONSOLE_COMMAND_GETCONFIG,
    CONSOLE_COMMAND_SETCONFIG,
    CONSOLE_COMMAND_RESETCONFIG,
    CONSOLE_COMMAND_GETSENSORS,
    CONSOLE_COMMAND_STATUS,
} console_command_id_t;

typedef struct {
    const char *name;
    const char *help;
    console_command_id_t id;
} console_command_entry_t;

typedef struct {
    const char *name;
    runtime_config_key_t key;
} console_runtime_config_entry_t;

static const console_command_entry_t s_commands[] = {
    {"setmotor", "Set raw diagnostic motor output: setmotor <motor_id> <pwm> <dir>", CONSOLE_COMMAND_SETMOTOR},
    {"jobrx", "Queue tray receive job: jobrx", CONSOLE_COMMAND_JOBRX},
    {"jobtx", "Queue tray transmit job: jobtx", CONSOLE_COMMAND_JOBTX},
    {"get_smstatus", "Get state-machine status: get_smstatus", CONSOLE_COMMAND_GET_SMSTATUS},
    {"stop", "Stop all motors: stop", CONSOLE_COMMAND_STOP},
    {"stopmotor", "Stop raw motor output: stopmotor <motor_id>", CONSOLE_COMMAND_STOPMOTOR},
    {"setposition", "Set PID target position: setposition <motor_id> <position>", CONSOLE_COMMAND_SETPOSITION},
    {"getposition", "Get current position: getposition <motor_id>", CONSOLE_COMMAND_GETPOSITION},
    {"pid_control", "Enable or disable PID ownership: pid_control <motor_id> <0|1>", CONSOLE_COMMAND_PID_CONTROL},
    {"setspeed", "Set PID target speed: setspeed <motor_id> <speed>", CONSOLE_COMMAND_SETSPEED},
    {"getspeed", "Get current speed: getspeed <motor_id>", CONSOLE_COMMAND_GETSPEED},
    {"get_pidmode", "Get active PID mode: get_pidmode <motor_id>", CONSOLE_COMMAND_GET_PIDMODE},
    {"set_pidmode", "Set active PID mode: set_pidmode <motor_id> <position|speed>", CONSOLE_COMMAND_SET_PIDMODE},
    {"setpid", "Set per-motor PID gains: setpid <motor_id> <kp_milli> <ki_milli> <kd_milli>", CONSOLE_COMMAND_SETPID},
    {"getpid", "Get per-motor PID gains: getpid <motor_id>", CONSOLE_COMMAND_GETPID},
    {"setoffset", "Set position offset: setoffset <motor_id> <offset>", CONSOLE_COMMAND_SETOFFSET},
    {"getconfig", "Read runtime config: getconfig [key]", CONSOLE_COMMAND_GETCONFIG},
    {"setconfig", "Set runtime config in RAM: setconfig <key> <value>", CONSOLE_COMMAND_SETCONFIG},
    {"resetconfig", "Reset runtime config to flash default: resetconfig <key>", CONSOLE_COMMAND_RESETCONFIG},
    {"getsensors", "Get current sensor states: getsensors <motor_id>", CONSOLE_COMMAND_GETSENSORS},
    {"status", "Show firmware, commands, and motors: status", CONSOLE_COMMAND_STATUS},
};

static const console_runtime_config_entry_t s_runtime_configs[] = {
    {"max_pwm", RUNTIME_CONFIG_MAX_PWM},
    {"position_tolerance_counts", RUNTIME_CONFIG_POSITION_TOLERANCE_COUNTS},
};

/* Parses a base-10 integer argument and rejects partial or overflowing values. */
static bool parse_int_arg(const char *text, int *out_value)
{
    char *end = NULL;
    long value = 0;

    if (text == NULL || out_value == NULL) {
        return false;
    }

    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        return false;
    }

    *out_value = (int)value;
    return true;
}

/* Converts state-machine job results to console-stable tokens. */
static const char *statemachine_result_text(statemachine_result_t result)
{
    switch (result) {
    case STATEMACHINE_RESULT_RX_DONE:
        return "RX_DONE";
    case STATEMACHINE_RESULT_TX_DONE:
        return "TX_DONE";
    case STATEMACHINE_RESULT_TRAY_ALREADY_PRESENT:
        return "TRAY_ALREADY_PRESENT";
    case STATEMACHINE_RESULT_TRAY_NOT_RECEIVED:
        return "TRAY_NOT_RECEIVED";
    case STATEMACHINE_RESULT_TRAY_TRANSFER_STUCK:
        return "TRAY_TRANSFER_STUCK";
    case STATEMACHINE_RESULT_NO_TRAY_PRESENT:
        return "NO_TRAY_PRESENT";
    case STATEMACHINE_RESULT_TRAY_HANDOFF_STUCK:
        return "TRAY_HANDOFF_STUCK";
    case STATEMACHINE_RESULT_EMERGENCY_STOP:
        return "EMERGENCY_STOP";
    case STATEMACHINE_RESULT_JOB_TIMEOUT:
        return "JOB_TIMEOUT";
    case STATEMACHINE_RESULT_JOB_REJECTED:
        return "JOB_REJECTED";
    }

    return "UNKNOWN";
}

/* Converts state-machine live status to console-stable tokens. */
static const char *statemachine_status_text(statemachine_status_t status)
{
    switch (status) {
    case STATEMACHINE_STATUS_IDLE:
        return "IDLE";
    case STATEMACHINE_STATUS_RECEIVE_WAITING_FOR_TRAY:
        return "RECEIVE_WAITING_FOR_TRAY";
    case STATEMACHINE_STATUS_RECEIVE_MOVING_TRAY:
        return "RECEIVE_MOVING_TRAY";
    case STATEMACHINE_STATUS_RECEIVE_TRAY_RECEIVED:
        return "RECEIVE_TRAY_RECEIVED";
    case STATEMACHINE_STATUS_TRANSMIT_TRANSMITTING_TRAY:
        return "TRANSMIT_TRANSMITTING_TRAY";
    case STATEMACHINE_STATUS_TRANSMIT_TRAY_HANDED_OFF:
        return "TRANSMIT_TRAY_HANDED_OFF";
    }

    return "UNKNOWN";
}

/* Handles every registered console command through one command table and switch. */
static int handle_console_command(int argc, char **argv)
{
    console_command_id_t command_id = 0;
    bool found_command = false;
    esp_err_t err = ESP_OK;

    if (argc <= 0 || argv == NULL || argv[0] == NULL) {
        printf("ERR BAD_ARGS\n");
        return 0;
    }

    /* Resolve argv[0] through the same command table used for registration. */
    for (size_t i = 0; i < sizeof(s_commands) / sizeof(s_commands[0]); i++) {
        if (strcmp(argv[0], s_commands[i].name) == 0) {
            command_id = s_commands[i].id;
            found_command = true;
            break;
        }
    }
    if (!found_command) {
        printf("ERR UNKNOWN_COMMAND\n");
        return 0;
    }

    switch (command_id) {
    case CONSOLE_COMMAND_SETMOTOR: {
        int pwm = 0;
        int direction = 0;

        /* setmotor forwards raw output directly to the hardware layer. */
        /* Parse the PWM and direction strings before calling the hardware API. */
        if (argc != 4 || !parse_int_arg(argv[2], &pwm) || !parse_int_arg(argv[3], &direction)) {
            printf("ERR BAD_ARGS\n");
            return 0;
        }

        /* Hand the parsed raw motor output to hardware.c. */
        err = pid_set_control(argv[1], false);
        if (err == ESP_OK) {
            err = set_motor(argv[1], pwm, direction);
        }
        if (err != ESP_OK) {
            printf("ERR %s\n", esp_err_to_name(err));
            return 0;
        }

        printf("OK SETMOTOR motor=%s pwm=%d dir=%d\n", argv[1], pwm, direction);
        return 0;
    }

    case CONSOLE_COMMAND_JOBRX:
    {
        statemachine_result_t result = STATEMACHINE_RESULT_JOB_REJECTED;

        if (argc != 1) {
            printf("ERR BAD_ARGS\n");
            return 0;
        }

        /* Run the receive job through statemachine.c and wait for its result. */
        result = statemachine_jobrx();
        printf("OK JOBRX result=%s\n", statemachine_result_text(result));
        return 0;
    }

    case CONSOLE_COMMAND_JOBTX:
    {
        statemachine_result_t result = STATEMACHINE_RESULT_JOB_REJECTED;

        if (argc != 1) {
            printf("ERR BAD_ARGS\n");
            return 0;
        }

        /* Run the transmit job through statemachine.c and wait for its result. */
        result = statemachine_jobtx();
        printf("OK JOBTX result=%s\n", statemachine_result_text(result));
        return 0;
    }

    case CONSOLE_COMMAND_GET_SMSTATUS:
    {
        statemachine_status_t status = STATEMACHINE_STATUS_IDLE;

        if (argc != 1) {
            printf("ERR BAD_ARGS\n");
            return 0;
        }

        /* Read the live state-machine status from statemachine.c. */
        status = statemachine_get_status();
        printf("OK STATUS state=%s\n", statemachine_status_text(status));
        return 0;
    }

    case CONSOLE_COMMAND_STOP:
        if (argc != 1) {
            printf("ERR BAD_ARGS\n");
            return 0;
        }

        for (int i = 0; i < APP_MOTOR_COUNT; i++) {
            const char *motor_id = NULL;

            if (hardware_get_motor_id(i, &motor_id) != ESP_OK) {
                continue;
            }
            (void)pid_set_control(motor_id, false);
            /* Hand each configured motor id to hardware.c so all PWM outputs are cut. */
            stop_motor(motor_id);
        }
        printf("OK STOP motors=%d\n", APP_MOTOR_COUNT);
        return 0;

    case CONSOLE_COMMAND_STOPMOTOR:
        /* stopmotor cuts PWM through the hardware layer immediately. */
        if (argc != 2) {
            printf("ERR BAD_ARGS\n");
            return 0;
        }

        /* Hand the stop request to hardware.c so PWM is cut immediately. */
        err = pid_set_control(argv[1], false);
        if (err != ESP_OK) {
            printf("ERR %s\n", esp_err_to_name(err));
            return 0;
        }
        stop_motor(argv[1]);
        printf("OK STOPMOTOR motor=%s\n", argv[1]);
        return 0;

    case CONSOLE_COMMAND_SETPOSITION: {
        int target_position = 0;
        bool PID_control = false;

        /* setposition publishes PID intent for the per-motor PID task. */
        /* Parse the target position string before calling the PID API. */
        if (argc != 3 || !parse_int_arg(argv[2], &target_position)) {
            printf("ERR BAD_ARGS\n");
            return 0;
        }
        err = pid_get_control(argv[1], &PID_control);
        if (err != ESP_OK) {
            printf("ERR %s\n", esp_err_to_name(err));
            return 0;
        }
        if (!PID_control) {
            printf("ERR PID_CONTROL_DISABLED\n");
            return 0;
        }

        /* Hand the parsed target position to pid.c. */
        err = set_position(argv[1], target_position);
        if (err == ESP_ERR_INVALID_STATE) {
            printf("ERR PID_CONTROL_DISABLED\n");
            return 0;
        }
        if (err != ESP_OK) {
            printf("ERR %s\n", esp_err_to_name(err));
            return 0;
        }

        printf("OK SETPOSITION motor=%s position=%d\n", argv[1], target_position);
        return 0;
    }

    case CONSOLE_COMMAND_GETPOSITION: {
        int position = 0;

        /* getposition reads the latest offset-corrected position snapshot. */
        if (argc != 2) {
            printf("ERR BAD_ARGS\n");
            return 0;
        }

        /* Ask pid.c for the latest position snapshot for this motor id. */
        err = get_position(argv[1], &position);
        if (err != ESP_OK) {
            printf("ERR %s\n", esp_err_to_name(err));
            return 0;
        }

        printf("OK POSITION motor=%s pos=%d\n", argv[1], position);
        return 0;
    }

    case CONSOLE_COMMAND_SETSPEED: {
        int target_speed = 0;

        /* setspeed exists so the new speed controller can be commissioned without raw shared-state writes. */
        if (argc != 3 || !parse_int_arg(argv[2], &target_speed)) {
            printf("ERR BAD_ARGS\n");
            return 0;
        }

        /* Hand the parsed target speed to pid.c, matching setposition's ownership boundary. */
        err = set_speed(argv[1], target_speed);
        if (err == ESP_ERR_INVALID_STATE) {
            printf("ERR PID_CONTROL_DISABLED\n");
            return 0;
        }
        if (err != ESP_OK) {
            printf("ERR %s\n", esp_err_to_name(err));
            return 0;
        }

        printf("OK SETSPEED motor=%s speed=%d\n", argv[1], target_speed);
        return 0;
    }

    case CONSOLE_COMMAND_GETSPEED: {
        int speed = 0;

        /* getspeed reports hardware_task()'s latest counts-per-second snapshot for tuning. */
        if (argc != 2) {
            printf("ERR BAD_ARGS\n");
            return 0;
        }

        /* Keep speed reads behind pid.c for symmetry with the existing position API. */
        err = get_speed(argv[1], &speed);
        if (err != ESP_OK) {
            printf("ERR %s\n", esp_err_to_name(err));
            return 0;
        }

        printf("OK SPEED motor=%s speed=%d\n", argv[1], speed);
        return 0;
    }

    case CONSOLE_COMMAND_GET_PIDMODE: {
        motor_pid_mode_t pid_mode = MOTOR_PID_MODE_POSITION;

        /* get_pidmode reads the controller selection through the PID owner API. */
        if (argc != 2) {
            printf("ERR BAD_ARGS\n");
            return 0;
        }
        err = pid_get_mode(argv[1], &pid_mode);
        if (err != ESP_OK) {
            printf("ERR %s\n", esp_err_to_name(err));
            return 0;
        }

        printf("OK PIDMODE motor=%s mode=%s\n", argv[1],
               pid_mode == MOTOR_PID_MODE_SPEED ? "speed" : "position");
        return 0;
    }

    case CONSOLE_COMMAND_SET_PIDMODE: {
        motor_pid_mode_t pid_mode = MOTOR_PID_MODE_POSITION;

        /* set_pidmode changes only the controller source so the driver can switch UI modes before sending targets. */
        if (argc != 3) {
            printf("ERR BAD_ARGS\n");
            return 0;
        }
        if (strcmp(argv[2], "position") == 0) {
            pid_mode = MOTOR_PID_MODE_POSITION;
        } else if (strcmp(argv[2], "speed") == 0) {
            pid_mode = MOTOR_PID_MODE_SPEED;
        } else {
            printf("ERR BAD_ARGS\n");
            return 0;
        }

        /* Reset PID memory in pid.c because the old mode may have different units. */
        err = pid_set_mode(argv[1], pid_mode);
        if (err != ESP_OK) {
            printf("ERR %s\n", esp_err_to_name(err));
            return 0;
        }

        printf("OK PIDMODE motor=%s mode=%s\n", argv[1],
               pid_mode == MOTOR_PID_MODE_SPEED ? "speed" : "position");
        return 0;
    }

    case CONSOLE_COMMAND_PID_CONTROL: {
        int enabled = 0;

        if (argc != 3 || !parse_int_arg(argv[2], &enabled) || (enabled != 0 && enabled != 1)) {
            printf("ERR BAD_ARGS\n");
            return 0;
        }

        err = pid_set_control(argv[1], enabled != 0);
        if (err != ESP_OK) {
            printf("ERR %s\n", esp_err_to_name(err));
            return 0;
        }

        printf("OK PID_CONTROL motor=%s enabled=%d\n", argv[1], enabled);
        return 0;
    }

    case CONSOLE_COMMAND_SETPID: {
        int kp_milli = 0;
        int ki_milli = 0;
        int kd_milli = 0;

        if (argc != 5 || !parse_int_arg(argv[2], &kp_milli) ||
            !parse_int_arg(argv[3], &ki_milli) || !parse_int_arg(argv[4], &kd_milli) ||
            kp_milli < 0 || ki_milli < 0 || kd_milli < 0) {
            printf("ERR BAD_ARGS\n");
            return 0;
        }

        /* Keep gain ownership in pid.c; console only parses text and prints the result. */
        err = set_pid_gains(argv[1], kp_milli, ki_milli, kd_milli);
        if (err != ESP_OK) {
            printf("ERR %s\n", esp_err_to_name(err));
            return 0;
        }

        printf("OK SETPID motor=%s kp_milli=%d ki_milli=%d kd_milli=%d\n",
               argv[1], kp_milli, ki_milli, kd_milli);
        return 0;
    }

    case CONSOLE_COMMAND_GETPID: {
        int kp_milli = 0;
        int ki_milli = 0;
        int kd_milli = 0;

        if (argc != 2) {
            printf("ERR BAD_ARGS\n");
            return 0;
        }

        /* Read through pid.c for the same reason setpid writes through pid.c. */
        err = get_pid_gains(argv[1], &kp_milli, &ki_milli, &kd_milli);
        if (err != ESP_OK) {
            printf("ERR %s\n", esp_err_to_name(err));
            return 0;
        }

        printf("OK PID motor=%s kp_milli=%d ki_milli=%d kd_milli=%d\n",
               argv[1], kp_milli, ki_milli, kd_milli);
        return 0;
    }

    case CONSOLE_COMMAND_GETSENSORS: {
        int upstream_sensor = 0;
        int downstream_sensor = 0;

        if (argc != 2) {
            printf("ERR BAD_ARGS\n");
            return 0;
        }
        err = hardware_get_sensors(argv[1], &upstream_sensor, &downstream_sensor);
        if (err != ESP_OK) {
            printf("ERR %s\n", esp_err_to_name(err));
            return 0;
        }

        printf("OK SENSORS motor=%s upstream=%d downstream=%d\n", argv[1], upstream_sensor, downstream_sensor);
        return 0;
    }

    case CONSOLE_COMMAND_SETOFFSET: {
        int offset = 0;

        /* setoffset adjusts the encoder-to-real-position relationship. */
        /* Parse the offset string before calling the PID API. */
        if (argc != 3 || !parse_int_arg(argv[2], &offset)) {
            printf("ERR BAD_ARGS\n");
            return 0;
        }

        /* Hand the parsed offset to pid.c. */
        err = set_offset(argv[1], offset);
        if (err != ESP_OK) {
            printf("ERR %s\n", esp_err_to_name(err));
            return 0;
        }

        printf("OK SETOFFSET motor=%s offset=%d\n", argv[1], offset);
        return 0;
    }

    case CONSOLE_COMMAND_GETCONFIG:
        /* getconfig translates console names to enum keys before calling runtime_config. */
        if (argc > 2) {
            printf("ERR BAD_ARGS\n");
            return 0;
        }
        if (argc == 2) {
            runtime_config_key_t key = RUNTIME_CONFIG_COUNT;
            int32_t value = 0;

            for (size_t i = 0; i < sizeof(s_runtime_configs) / sizeof(s_runtime_configs[0]); i++) {
                if (strcmp(argv[1], s_runtime_configs[i].name) == 0) {
                    key = s_runtime_configs[i].key;
                    break;
                }
            }
            if (key == RUNTIME_CONFIG_COUNT) {
                printf("ERR BAD_CONFIG_KEY\n");
                return 0;
            }
            /* Read the enum-keyed config value from runtime_config.c. */
            err = runtime_config_get(key, &value);
            if (err != ESP_OK) {
                printf("ERR %s\n", esp_err_to_name(err));
                return 0;
            }

            printf("CONFIG %s %ld\n", argv[1], (long)value);
            return 0;
        }

        for (size_t i = 0; i < sizeof(s_runtime_configs) / sizeof(s_runtime_configs[0]); i++) {
            const runtime_config_key_t key = s_runtime_configs[i].key;
            int32_t value = 0;

            /* Read this enum-keyed config value from runtime_config.c. */
            err = runtime_config_get(key, &value);
            if (err != ESP_OK) {
                printf("ERR %s\n", esp_err_to_name(err));
                return 0;
            }
            printf("CONFIG %s %ld\n", s_runtime_configs[i].name, (long)value);
        }
        return 0;

    case CONSOLE_COMMAND_SETCONFIG: {
        runtime_config_key_t key = RUNTIME_CONFIG_COUNT;
        const char *config_name = NULL;
        int value = 0;

        /* setconfig updates RAM only; flash defaults remain compiled into runtime_config.c. */
        /* Parse the new config value before calling runtime_config.c. */
        if (argc != 3 || !parse_int_arg(argv[2], &value)) {
            printf("ERR BAD_ARGS\n");
            return 0;
        }
        for (size_t i = 0; i < sizeof(s_runtime_configs) / sizeof(s_runtime_configs[0]); i++) {
            if (strcmp(argv[1], s_runtime_configs[i].name) == 0) {
                key = s_runtime_configs[i].key;
                config_name = s_runtime_configs[i].name;
                break;
            }
        }
        if (key == RUNTIME_CONFIG_COUNT) {
            printf("ERR BAD_CONFIG_KEY\n");
            return 0;
        }

        /* Write the enum-keyed value into runtime_config.c RAM state. */
        err = runtime_config_set(key, value);
        if (err != ESP_OK) {
            printf("ERR %s\n", esp_err_to_name(err));
            return 0;
        }

        printf("OK SETCONFIG %s %d\n", config_name, value);
        return 0;
    }

    case CONSOLE_COMMAND_RESETCONFIG: {
        runtime_config_key_t key = RUNTIME_CONFIG_COUNT;
        const char *config_name = NULL;

        /* resetconfig restores one named runtime config to its flash default in RAM. */
        if (argc != 2) {
            printf("ERR BAD_ARGS\n");
            return 0;
        }
        for (size_t i = 0; i < sizeof(s_runtime_configs) / sizeof(s_runtime_configs[0]); i++) {
            if (strcmp(argv[1], s_runtime_configs[i].name) == 0) {
                key = s_runtime_configs[i].key;
                config_name = s_runtime_configs[i].name;
                break;
            }
        }
        if (key == RUNTIME_CONFIG_COUNT) {
            printf("ERR BAD_CONFIG_KEY\n");
            return 0;
        }

        /* Reset the enum-keyed value in runtime_config.c RAM state. */
        err = runtime_config_reset(key);
        if (err != ESP_OK) {
            printf("ERR %s\n", esp_err_to_name(err));
            return 0;
        }

        printf("OK RESETCONFIG %s\n", config_name);
        return 0;
    }

    case CONSOLE_COMMAND_STATUS:
        if (argc != 1) {
            printf("ERR BAD_ARGS\n");
            return 0;
        }

        printf("STATUS app_name=%s\n", APP_MOTOR_APP_NAME);
        printf("STATUS machine_id=%s\n", APP_MOTOR_MACHINE_ID);
        printf("STATUS mqtt_client_id=%s\n", APP_MOTOR_MQTT_CLIENT_ID);
        printf("STATUS topic_name=%s\n", APP_MOTOR_TOPIC_NAME);
        printf("STATUS wifi_ssid=%s\n", APP_MOTOR_WIFI_SSID);
        printf("STATUS wifi_pass=%s\n", APP_MOTOR_WIFI_PASS);
        printf("STATUS mqtt_uri=%s\n", APP_MOTOR_MQTT_URI);
        for (size_t i = 0; i < sizeof(s_commands) / sizeof(s_commands[0]); i++) {
            printf("COMMAND %s - %s\n", s_commands[i].name, s_commands[i].help);
        }
        for (int i = 0; i < APP_MOTOR_COUNT; i++) {
            const char *motor_id = NULL;
            int kp_milli = 0;
            int ki_milli = 0;
            int kd_milli = 0;

            if (hardware_get_motor_id(i, &motor_id) != ESP_OK) {
                continue;
            }
            /* Status reports per-motor tuning without reaching around the PID API boundary. */
            (void)get_pid_gains(motor_id, &kp_milli, &ki_milli, &kd_milli);
            printf("MOTOR %s kp_milli=%d ki_milli=%d kd_milli=%d\n", motor_id, kp_milli, ki_milli, kd_milli);
        }
        return 0;
    }

    printf("ERR UNKNOWN_COMMAND\n");
    return 0;
}

/* Registers every console command from the command table. */
static esp_err_t register_console_commands(void)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_console_register_help_command());

    for (size_t i = 0; i < sizeof(s_commands) / sizeof(s_commands[0]); i++) {
        const esp_console_cmd_t command = {
            .command = s_commands[i].name,
            .help = s_commands[i].help,
            .hint = NULL,
            .func = &handle_console_command,
        };

        ESP_RETURN_ON_ERROR(esp_console_cmd_register(&command), "console", "register command");
    }

    return ESP_OK;
}

/* Initializes console support and attaches registered commands. */
esp_err_t console_init(void)
{
    const esp_console_config_t console_config = {
        .max_cmdline_args = CONSOLE_MAX_ARGS,
        .max_cmdline_length = CONSOLE_MAX_LINE_LENGTH,
    };

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    usb_serial_jtag_driver_config_t usb_serial_jtag_config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();

    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_CR);
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);
    if (!usb_serial_jtag_is_driver_installed()) {
        ESP_RETURN_ON_ERROR(usb_serial_jtag_driver_install(&usb_serial_jtag_config),
                            "console", "install USB serial JTAG driver");
    }
    usb_serial_jtag_vfs_use_driver();
#endif

    ESP_RETURN_ON_ERROR(esp_console_init(&console_config), "console", "initialize console");
    /* Link console initialization to this module's command registration. */
    return register_console_commands();
}

/* Runs the console loop and executes registered commands from stdin. */
void console_task(void *arg)
{
    (void)arg;

    /* Print the boot-ready marker used during bring-up. */
    printf("READY %s\n", APP_MOTOR_APP_NAME);
    linenoiseSetMultiLine(1);

    while (true) {
        int command_result = 0;
        char *line = linenoise("> ");

        if (line == NULL) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (strlen(line) > 0) {
            linenoiseHistoryAdd(line);
            const esp_err_t err = esp_console_run(line, &command_result);
            if (err == ESP_ERR_NOT_FOUND) {
                printf("ERR UNKNOWN_COMMAND\n");
            } else if (err == ESP_ERR_INVALID_ARG) {
                printf("ERR BAD_ARGS\n");
            } else if (err != ESP_OK) {
                printf("ERR %s\n", esp_err_to_name(err));
            }
            fflush(stdout);
        }

        linenoiseFree(line);
    }
}
