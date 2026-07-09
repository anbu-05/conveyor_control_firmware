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
#include "shared/app_state.h"
#include "tasks/hardware.h"
#include "tasks/pid.h"

#define CONSOLE_MAX_ARGS 8
#define CONSOLE_MAX_LINE_LENGTH 256
#define CONSOLE_PID_GAIN_SCALE 1000.0f
#define CONSOLE_IS_PID_GAIN_KEY(key) ((key) == RUNTIME_CONFIG_PID_KP_MILLI || \
                                      (key) == RUNTIME_CONFIG_PID_KI_MILLI || \
                                      (key) == RUNTIME_CONFIG_PID_KD_MILLI)

typedef enum {
    CONSOLE_COMMAND_SETMOTOR,
    CONSOLE_COMMAND_STOP,
    CONSOLE_COMMAND_STOPMOTOR,
    CONSOLE_COMMAND_SETPOSITION,
    CONSOLE_COMMAND_GETPOSITION,
    CONSOLE_COMMAND_SETOFFSET,
    CONSOLE_COMMAND_SETK,
    CONSOLE_COMMAND_GETCONFIG,
    CONSOLE_COMMAND_SETCONFIG,
    CONSOLE_COMMAND_RESETCONFIG,
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
    {"setoffset", "Set position offset: setoffset <motor_id> <offset>", CONSOLE_COMMAND_SETOFFSET},
    {"setk", "Set live PID gains: setk <motor_id> <kp> <ki> <kd>", CONSOLE_COMMAND_SETK},
    {"getconfig", "Read runtime config: getconfig [key]", CONSOLE_COMMAND_GETCONFIG},
    {"setconfig", "Set and save runtime config: setconfig <key> <value>", CONSOLE_COMMAND_SETCONFIG},
    {"resetconfig", "Reset and save runtime config: resetconfig <key>", CONSOLE_COMMAND_RESETCONFIG},
    {"status", "Show firmware, commands, and motors: status", CONSOLE_COMMAND_STATUS},
};

static const console_runtime_config_entry_t s_runtime_configs[] = {
    {"pid_kp_milli", RUNTIME_CONFIG_PID_KP_MILLI},
    {"pid_ki_milli", RUNTIME_CONFIG_PID_KI_MILLI},
    {"pid_kd_milli", RUNTIME_CONFIG_PID_KD_MILLI},
    {"max_pwm", RUNTIME_CONFIG_MAX_PWM},
    {"min_start_pwm", RUNTIME_CONFIG_MIN_START_PWM},
    {"reference_speed_counts_per_sec", RUNTIME_CONFIG_REFERENCE_SPEED_COUNTS_PER_SEC},
    {"positive_speed_counts_per_sec", RUNTIME_CONFIG_POSITIVE_SPEED_COUNTS_PER_SEC},
    {"negative_speed_counts_per_sec", RUNTIME_CONFIG_NEGATIVE_SPEED_COUNTS_PER_SEC},
    {"sensor_seek_speed_counts_per_sec", RUNTIME_CONFIG_SENSOR_SEEK_SPEED_COUNTS_PER_SEC},
    {"max_speed_counts_per_sec", RUNTIME_CONFIG_MAX_SPEED_COUNTS_PER_SEC},
    {"position_tolerance_counts", RUNTIME_CONFIG_POSITION_TOLERANCE_COUNTS},
    {"reference_timeout_ms", RUNTIME_CONFIG_REFERENCE_TIMEOUT_MS},
    {"positive_timeout_ms", RUNTIME_CONFIG_POSITIVE_TIMEOUT_MS},
    {"negative_timeout_ms", RUNTIME_CONFIG_NEGATIVE_TIMEOUT_MS},
    {"stall_check_ms", RUNTIME_CONFIG_STALL_CHECK_MS},
    {"stall_min_delta_counts", RUNTIME_CONFIG_STALL_MIN_DELTA_COUNTS},
    {"direction_check_delay_ms", RUNTIME_CONFIG_DIRECTION_CHECK_DELAY_MS},
    {"limit_switch_qualify_ms", RUNTIME_CONFIG_LIMIT_SWITCH_QUALIFY_MS},
    {"mqtt_status_period_ms", RUNTIME_CONFIG_MQTT_STATUS_PERIOD_MS},
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

/* Parses a floating-point argument for live PID gain commands. */
static bool parse_float_arg(const char *text, float *out_value)
{
    char *end = NULL;
    float value = 0.0f;

    if (text == NULL || out_value == NULL) {
        return false;
    }

    errno = 0;
    value = strtof(text, &end);
    if (errno != 0 || end == text || *end != '\0') {
        return false;
    }

    *out_value = value;
    return true;
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
        err = set_motor(argv[1], pwm, direction);
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
        stop_motor(argv[1]);
        printf("OK STOPMOTOR motor=%s\n", argv[1]);
        return 0;

    case CONSOLE_COMMAND_SETPOSITION: {
        int target_position = 0;

        /* setposition publishes PID intent for the per-motor PID task. */
        /* Parse the target position string before calling the PID API. */
        if (argc != 3 || !parse_int_arg(argv[2], &target_position)) {
            printf("ERR BAD_ARGS\n");
            return 0;
        }

        /* Hand the parsed target position to pid.c. */
        err = set_position(argv[1], target_position);
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

    case CONSOLE_COMMAND_SETK: {
        float kp = 0.0f;
        float ki = 0.0f;
        float kd = 0.0f;

        /* setk updates live gains only; setconfig owns persisted gain values. */
        if (argc != 5 ||
            /* Parse kp before calling the live PID gain API. */
            !parse_float_arg(argv[2], &kp) ||
            /* Parse ki before calling the live PID gain API. */
            !parse_float_arg(argv[3], &ki) ||
            /* Parse kd before calling the live PID gain API. */
            !parse_float_arg(argv[4], &kd)) {
            printf("ERR BAD_ARGS\n");
            return 0;
        }

        /* Hand the parsed live gains to pid.c. */
        err = setk(argv[1], kp, ki, kd);
        if (err != ESP_OK) {
            printf("ERR %s\n", esp_err_to_name(err));
            return 0;
        }

        printf("OK SETK motor=%s kp=%.3f ki=%.3f kd=%.3f\n", argv[1], kp, ki, kd);
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

        /* setconfig updates RAM, persists to NVS, and refreshes live PID gains if needed. */
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
        if (err == ESP_OK) {
            /* Persist the same enum-keyed runtime config value to NVS. */
            err = runtime_config_store_nvs(key);
        }
        if (err == ESP_OK && CONSOLE_IS_PID_GAIN_KEY(key)) {
            int32_t kp_milli = 0;
            int32_t ki_milli = 0;
            int32_t kd_milli = 0;

            /* PID gains are persisted as milli-units but consumed as live floats. */
            /* Read persisted kp milli-units before refreshing live PID gains. */
            err = runtime_config_get(RUNTIME_CONFIG_PID_KP_MILLI, &kp_milli);
            if (err == ESP_OK) {
                /* Read persisted ki milli-units before refreshing live PID gains. */
                err = runtime_config_get(RUNTIME_CONFIG_PID_KI_MILLI, &ki_milli);
            }
            if (err == ESP_OK) {
                /* Read persisted kd milli-units before refreshing live PID gains. */
                err = runtime_config_get(RUNTIME_CONFIG_PID_KD_MILLI, &kd_milli);
            }
            if (err == ESP_OK) {
                for (int i = 0; i < APP_MOTOR_COUNT; i++) {
                    /* Hand the refreshed persisted gains to each motor's PID state. */
                    err = setk(motors[i].id,
                               (float)kp_milli / CONSOLE_PID_GAIN_SCALE,
                               (float)ki_milli / CONSOLE_PID_GAIN_SCALE,
                               (float)kd_milli / CONSOLE_PID_GAIN_SCALE);
                    if (err != ESP_OK) {
                        break;
                    }
                }
            }
        }
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

        /* resetconfig resets one named runtime config and persists the default. */
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
        if (err == ESP_OK) {
            /* Persist the reset enum-keyed runtime config value to NVS. */
            err = runtime_config_store_nvs(key);
        }
        if (err == ESP_OK && CONSOLE_IS_PID_GAIN_KEY(key)) {
            int32_t kp_milli = 0;
            int32_t ki_milli = 0;
            int32_t kd_milli = 0;

            /* Refresh live motor PID gains when persisted PID defaults change. */
            /* Read reset kp milli-units before refreshing live PID gains. */
            err = runtime_config_get(RUNTIME_CONFIG_PID_KP_MILLI, &kp_milli);
            if (err == ESP_OK) {
                /* Read reset ki milli-units before refreshing live PID gains. */
                err = runtime_config_get(RUNTIME_CONFIG_PID_KI_MILLI, &ki_milli);
            }
            if (err == ESP_OK) {
                /* Read reset kd milli-units before refreshing live PID gains. */
                err = runtime_config_get(RUNTIME_CONFIG_PID_KD_MILLI, &kd_milli);
            }
            if (err == ESP_OK) {
                for (int i = 0; i < APP_MOTOR_COUNT; i++) {
                    /* Hand the reset persisted gains to each motor's PID state. */
                    err = setk(motors[i].id,
                               (float)kp_milli / CONSOLE_PID_GAIN_SCALE,
                               (float)ki_milli / CONSOLE_PID_GAIN_SCALE,
                               (float)kd_milli / CONSOLE_PID_GAIN_SCALE);
                    if (err != ESP_OK) {
                        break;
                    }
                }
            }
        }
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
