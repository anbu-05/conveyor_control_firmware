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
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "linenoise/linenoise.h"
#include "sdkconfig.h"
#include "shared/app_state.h"
#include "tasks/hardware.h"
#include "tasks/pid.h"

#define CONSOLE_MAX_ARGS 8
#define CONSOLE_MAX_LINE_LENGTH 256

typedef enum {
    CONSOLE_COMMAND_SETMOTOR,
    CONSOLE_COMMAND_STOP,
    CONSOLE_COMMAND_STOPMOTOR,
    CONSOLE_COMMAND_SETPOSITION,
    CONSOLE_COMMAND_GETPOSITION,
    CONSOLE_COMMAND_POSITIONCONTROL,
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
    {"setmotor", "Set raw motor output: setmotor <motor_id> <pwm> <dir>", CONSOLE_COMMAND_SETMOTOR},
    {"stop", "Stop all motors: stop", CONSOLE_COMMAND_STOP},
    {"stopmotor", "Stop raw motor output: stopmotor <motor_id>", CONSOLE_COMMAND_STOPMOTOR},
    {"setposition", "Set PID target position: setposition <motor_id> <position>", CONSOLE_COMMAND_SETPOSITION},
    {"getposition", "Get current position: getposition <motor_id>", CONSOLE_COMMAND_GETPOSITION},
    {"positioncontrol", "Enable or disable PID position control: positioncontrol <motor_id> <0|1>", CONSOLE_COMMAND_POSITIONCONTROL},
    {"setoffset", "Set position offset: setoffset <motor_id> <offset>", CONSOLE_COMMAND_SETOFFSET},
    {"getconfig", "Read runtime config: getconfig [key]", CONSOLE_COMMAND_GETCONFIG},
    {"setconfig", "Set runtime config in RAM: setconfig <key> <value>", CONSOLE_COMMAND_SETCONFIG},
    {"resetconfig", "Reset runtime config to flash default: resetconfig <key>", CONSOLE_COMMAND_RESETCONFIG},
    {"getsensors", "Get current sensor states: getsensors <motor_id>", CONSOLE_COMMAND_GETSENSORS},
    {"status", "Show firmware, commands, and motors: status", CONSOLE_COMMAND_STATUS},
};

static const console_runtime_config_entry_t s_runtime_configs[] = {
    {"pid_kp_milli", RUNTIME_CONFIG_PID_KP_MILLI},
    {"pid_ki_milli", RUNTIME_CONFIG_PID_KI_MILLI},
    {"pid_kd_milli", RUNTIME_CONFIG_PID_KD_MILLI},
    {"max_pwm", RUNTIME_CONFIG_MAX_PWM},
    {"max_speed_counts_per_sec", RUNTIME_CONFIG_MAX_SPEED_COUNTS_PER_SEC},
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

/* Resolves a console motor id to the shared motor array index. */
static int find_motor_index(const char *motor_id)
{
    for (int i = 0; i < APP_MOTOR_COUNT; i++) {
        if (motor_id != NULL && strcmp(motors[i].id, motor_id) == 0) {
            return i;
        }
    }

    return -1;
}

/* Enables/disables PID ownership for one motor from console commands. */
static esp_err_t set_console_position_control(const char *motor_id, bool enabled)
{
    int motor_index = find_motor_index(motor_id);

    if (motor_index < 0) {
        return ESP_ERR_NOT_FOUND;
    }

    if (motor_mutex != NULL) {
        xSemaphoreTake(motor_mutex, portMAX_DELAY);
    }
    motors[motor_index].position_control = enabled;
    motors[motor_index].target_position = motors[motor_index].current_position;
    motors[motor_index].integral = 0.0f;
    motors[motor_index].previous_error = 0.0f;
    motors[motor_index].has_previous_error = false;
    if (motor_mutex != NULL) {
        xSemaphoreGive(motor_mutex);
    }

    return ESP_OK;
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
        err = set_console_position_control(argv[1], false);
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

    case CONSOLE_COMMAND_STOP:
        if (argc != 1) {
            printf("ERR BAD_ARGS\n");
            return 0;
        }

        for (int i = 0; i < APP_MOTOR_COUNT; i++) {
            (void)set_console_position_control(motors[i].id, false);
            /* Hand each configured motor id to hardware.c so all PWM outputs are cut. */
            stop_motor(motors[i].id);
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
        err = set_console_position_control(argv[1], false);
        if (err != ESP_OK) {
            printf("ERR %s\n", esp_err_to_name(err));
            return 0;
        }
        stop_motor(argv[1]);
        printf("OK STOPMOTOR motor=%s\n", argv[1]);
        return 0;

    case CONSOLE_COMMAND_SETPOSITION: {
        int motor_index = -1;
        int target_position = 0;
        bool position_control = false;

        /* setposition publishes PID intent for the per-motor PID task. */
        /* Parse the target position string before calling the PID API. */
        if (argc != 3 || !parse_int_arg(argv[2], &target_position)) {
            printf("ERR BAD_ARGS\n");
            return 0;
        }
        motor_index = find_motor_index(argv[1]);
        if (motor_index < 0) {
            printf("ERR %s\n", esp_err_to_name(ESP_ERR_NOT_FOUND));
            return 0;
        }

        if (motor_mutex != NULL) {
            xSemaphoreTake(motor_mutex, portMAX_DELAY);
        }
        position_control = motors[motor_index].position_control;
        if (motor_mutex != NULL) {
            xSemaphoreGive(motor_mutex);
        }
        if (!position_control) {
            printf("ERR POSITION_CONTROL_DISABLED\n");
            return 0;
        }

        /* Hand the parsed target position to pid.c. */
        err = set_position(argv[1], target_position);
        if (err == ESP_ERR_INVALID_STATE) {
            printf("ERR POSITION_CONTROL_DISABLED\n");
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

    case CONSOLE_COMMAND_POSITIONCONTROL: {
        int enabled = 0;

        if (argc != 3 || !parse_int_arg(argv[2], &enabled) || (enabled != 0 && enabled != 1)) {
            printf("ERR BAD_ARGS\n");
            return 0;
        }

        err = set_console_position_control(argv[1], enabled != 0);
        if (err != ESP_OK) {
            printf("ERR %s\n", esp_err_to_name(err));
            return 0;
        }

        printf("OK POSITIONCONTROL motor=%s enabled=%d\n", argv[1], enabled);
        return 0;
    }

    case CONSOLE_COMMAND_GETSENSORS: {
        int motor_index = -1;
        int upstream_sensor = 0;
        int downstream_sensor = 0;

        if (argc != 2) {
            printf("ERR BAD_ARGS\n");
            return 0;
        }
        motor_index = find_motor_index(argv[1]);
        if (motor_index < 0) {
            printf("ERR %s\n", esp_err_to_name(ESP_ERR_NOT_FOUND));
            return 0;
        }

        if (motor_mutex != NULL) {
            xSemaphoreTake(motor_mutex, portMAX_DELAY);
        }
        upstream_sensor = motors[motor_index].upstream_sensor;
        downstream_sensor = motors[motor_index].downstream_sensor;
        if (motor_mutex != NULL) {
            xSemaphoreGive(motor_mutex);
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
            printf("MOTOR %s\n", motors[i].id);
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
