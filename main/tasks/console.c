/*
 * ESP console task.
 * This task registers simple commissioning commands and forwards each command to
 * the public hardware/PID APIs instead of owning motor behavior directly.
 */

#include "tasks/console.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config/config.h"
#include "config/runtime_config.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "linenoise/linenoise.h"
#include "shared/app_state.h"
#include "tasks/hardware.h"
#include "tasks/pid.h"

#define CONSOLE_MAX_ARGS 8
#define CONSOLE_MAX_LINE_LENGTH 256
#define CONSOLE_PID_GAIN_SCALE 1000.0f

typedef struct {
    const char *name;
    runtime_config_key_t key;
} console_config_key_t;

static const console_config_key_t s_config_keys[] = {
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

/* Maps a serial config key name to the runtime_config enum value. */
static bool config_key_from_name(const char *name, runtime_config_key_t *out_key)
{
    if (name == NULL || out_key == NULL) {
        return false;
    }

    /* Keep lookup local to console until the config module needs public names. */
    for (size_t i = 0; i < sizeof(s_config_keys) / sizeof(s_config_keys[0]); i++) {
        if (strcmp(s_config_keys[i].name, name) == 0) {
            *out_key = s_config_keys[i].key;
            return true;
        }
    }

    return false;
}

/* Returns the serial name for a runtime config key. */
static const char *config_name_from_key(runtime_config_key_t key)
{
    /* This reverse mapping keeps all command output spelling in one table. */
    for (size_t i = 0; i < sizeof(s_config_keys) / sizeof(s_config_keys[0]); i++) {
        if (s_config_keys[i].key == key) {
            return s_config_keys[i].name;
        }
    }

    return "unknown";
}

/* Returns true when a runtime key affects the live PID gain fields. */
static bool is_pid_gain_key(runtime_config_key_t key)
{
    return key == RUNTIME_CONFIG_PID_KP_MILLI ||
           key == RUNTIME_CONFIG_PID_KI_MILLI ||
           key == RUNTIME_CONFIG_PID_KD_MILLI;
}

/* Applies persisted PID milli-unit config values to every configured motor. */
static esp_err_t apply_runtime_pid_gains(void)
{
    int32_t kp_milli = 0;
    int32_t ki_milli = 0;
    int32_t kd_milli = 0;

    /* Read all three values first so every motor gets the same gain snapshot. */
    ESP_RETURN_ON_ERROR(runtime_config_get(RUNTIME_CONFIG_PID_KP_MILLI, &kp_milli),
                        "console", "read pid kp");
    ESP_RETURN_ON_ERROR(runtime_config_get(RUNTIME_CONFIG_PID_KI_MILLI, &ki_milli),
                        "console", "read pid ki");
    ESP_RETURN_ON_ERROR(runtime_config_get(RUNTIME_CONFIG_PID_KD_MILLI, &kd_milli),
                        "console", "read pid kd");

    /* Convert persisted milli-units into the live float gains used by PID tasks. */
    const float kp = (float)kp_milli / CONSOLE_PID_GAIN_SCALE;
    const float ki = (float)ki_milli / CONSOLE_PID_GAIN_SCALE;
    const float kd = (float)kd_milli / CONSOLE_PID_GAIN_SCALE;

    for (int i = 0; i < APP_MOTOR_COUNT; i++) {
        ESP_RETURN_ON_ERROR(setk(motors[i].id, kp, ki, kd), "console", "apply pid gains");
    }

    return ESP_OK;
}

/* Parses a base-10 integer argument and rejects partial or overflowing values. */
static bool parse_int_arg(const char *text, int *out_value)
{
    char *end = NULL;
    long value = 0;

    if (text == NULL || out_value == NULL) {
        return false;
    }

    /* strtol provides strict validation that atoi cannot provide. */
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

    /* strtof keeps gain parsing explicit and rejects trailing junk. */
    errno = 0;
    value = strtof(text, &end);
    if (errno != 0 || end == text || *end != '\0') {
        return false;
    }

    *out_value = value;
    return true;
}

/* Handles: setmotor <motor_id> <pwm> <dir>. */
static int handle_setmotor(int argc, char **argv)
{
    int pwm = 0;
    int direction = 0;

    if (argc != 4 || !parse_int_arg(argv[2], &pwm) || !parse_int_arg(argv[3], &direction)) {
        printf("ERR BAD_ARGS\n");
        return 0;
    }

    /* Forward raw output to the hardware layer; console does not touch app_state. */
    const esp_err_t err = set_motor(argv[1], pwm, direction);
    if (err != ESP_OK) {
        printf("ERR %s\n", esp_err_to_name(err));
        return 0;
    }

    printf("OK SETMOTOR motor=%s pwm=%d dir=%d\n", argv[1], pwm, direction);
    return 0;
}

/* Handles: stopmotor <motor_id>. */
static int handle_stopmotor(int argc, char **argv)
{
    if (argc != 2) {
        printf("ERR BAD_ARGS\n");
        return 0;
    }

    /* Stop is intentionally direct so commissioning can cut PWM immediately. */
    stop_motor(argv[1]);
    printf("OK STOPMOTOR motor=%s\n", argv[1]);
    return 0;
}

/* Handles: setposition <motor_id> <position>. */
static int handle_setposition(int argc, char **argv)
{
    int target_position = 0;

    if (argc != 3 || !parse_int_arg(argv[2], &target_position)) {
        printf("ERR BAD_ARGS\n");
        return 0;
    }

    /* Publish PID intent; the matching per-motor PID task reads it next tick. */
    const esp_err_t err = set_position(argv[1], target_position);
    if (err != ESP_OK) {
        printf("ERR %s\n", esp_err_to_name(err));
        return 0;
    }

    printf("OK SETPOSITION motor=%s position=%d\n", argv[1], target_position);
    return 0;
}

/* Handles: getposition <motor_id>. */
static int handle_getposition(int argc, char **argv)
{
    int position = 0;

    if (argc != 2) {
        printf("ERR BAD_ARGS\n");
        return 0;
    }

    /* Read the hardware-published, offset-corrected position through PID API. */
    const esp_err_t err = get_position(argv[1], &position);
    if (err != ESP_OK) {
        printf("ERR %s\n", esp_err_to_name(err));
        return 0;
    }

    printf("OK POSITION motor=%s pos=%d\n", argv[1], position);
    return 0;
}

/* Handles: setoffset <motor_id> <offset>. */
static int handle_setoffset(int argc, char **argv)
{
    int offset = 0;

    if (argc != 3 || !parse_int_arg(argv[2], &offset)) {
        printf("ERR BAD_ARGS\n");
        return 0;
    }

    /* Update encoder-to-position offset without resetting the hardware counter. */
    const esp_err_t err = set_offset(argv[1], offset);
    if (err != ESP_OK) {
        printf("ERR %s\n", esp_err_to_name(err));
        return 0;
    }

    printf("OK SETOFFSET motor=%s offset=%d\n", argv[1], offset);
    return 0;
}

/* Handles: setk <motor_id> <kp> <ki> <kd>. */
static int handle_setk(int argc, char **argv)
{
    float kp = 0.0f;
    float ki = 0.0f;
    float kd = 0.0f;

    if (argc != 5 ||
        !parse_float_arg(argv[2], &kp) ||
        !parse_float_arg(argv[3], &ki) ||
        !parse_float_arg(argv[4], &kd)) {
        printf("ERR BAD_ARGS\n");
        return 0;
    }

    /* Update live gains only; persistence remains owned by runtime config later. */
    const esp_err_t err = setk(argv[1], kp, ki, kd);
    if (err != ESP_OK) {
        printf("ERR %s\n", esp_err_to_name(err));
        return 0;
    }

    printf("OK SETK motor=%s kp=%.3f ki=%.3f kd=%.3f\n", argv[1], kp, ki, kd);
    return 0;
}

/* Handles: getconfig [key]. */
static int handle_getconfig(int argc, char **argv)
{
    if (argc > 2) {
        printf("ERR BAD_ARGS\n");
        return 0;
    }

    if (argc == 2) {
        runtime_config_key_t key = RUNTIME_CONFIG_COUNT;
        int32_t value = 0;

        /* Single-key mode keeps command output small during tuning. */
        if (!config_key_from_name(argv[1], &key)) {
            printf("ERR BAD_CONFIG_KEY\n");
            return 0;
        }
        const esp_err_t err = runtime_config_get(key, &value);
        if (err != ESP_OK) {
            printf("ERR %s\n", esp_err_to_name(err));
            return 0;
        }

        printf("CONFIG %s %ld\n", config_name_from_key(key), (long)value);
        return 0;
    }

    /* No-argument mode prints every editable runtime config value. */
    for (size_t i = 0; i < sizeof(s_config_keys) / sizeof(s_config_keys[0]); i++) {
        int32_t value = 0;
        const esp_err_t err = runtime_config_get(s_config_keys[i].key, &value);
        if (err != ESP_OK) {
            printf("ERR %s\n", esp_err_to_name(err));
            return 0;
        }
        printf("CONFIG %s %ld\n", s_config_keys[i].name, (long)value);
    }

    return 0;
}

/* Handles: setconfig <key> <value>. */
static int handle_setconfig(int argc, char **argv)
{
    runtime_config_key_t key = RUNTIME_CONFIG_COUNT;
    int value = 0;

    if (argc != 3 || !parse_int_arg(argv[2], &value)) {
        printf("ERR BAD_ARGS\n");
        return 0;
    }
    if (!config_key_from_name(argv[1], &key)) {
        printf("ERR BAD_CONFIG_KEY\n");
        return 0;
    }

    /* Update RAM first, then persist the accepted value to NVS. */
    esp_err_t err = runtime_config_set(key, value);
    if (err == ESP_OK) {
        err = runtime_config_store_nvs(key);
    }
    if (err == ESP_OK && is_pid_gain_key(key)) {
        err = apply_runtime_pid_gains();
    }
    if (err != ESP_OK) {
        printf("ERR %s\n", esp_err_to_name(err));
        return 0;
    }

    printf("OK SETCONFIG %s %d\n", config_name_from_key(key), value);
    return 0;
}

/* Handles: resetconfig [key]. */
static int handle_resetconfig(int argc, char **argv)
{
    if (argc > 2) {
        printf("ERR BAD_ARGS\n");
        return 0;
    }

    if (argc == 2) {
        runtime_config_key_t key = RUNTIME_CONFIG_COUNT;

        /* Reset one key to its compiled default and persist that default. */
        if (!config_key_from_name(argv[1], &key)) {
            printf("ERR BAD_CONFIG_KEY\n");
            return 0;
        }

        esp_err_t err = runtime_config_load_default(key);
        if (err == ESP_OK) {
            err = runtime_config_store_nvs(key);
        }
        if (err == ESP_OK && is_pid_gain_key(key)) {
            err = apply_runtime_pid_gains();
        }
        if (err != ESP_OK) {
            printf("ERR %s\n", esp_err_to_name(err));
            return 0;
        }

        printf("OK RESETCONFIG %s\n", config_name_from_key(key));
        return 0;
    }

    /* Reset every key to compiled defaults and persist the full runtime config. */
    runtime_config_load_defaults();
    esp_err_t err = runtime_config_store_all_nvs();
    if (err == ESP_OK) {
        err = apply_runtime_pid_gains();
    }
    if (err != ESP_OK) {
        printf("ERR %s\n", esp_err_to_name(err));
        return 0;
    }

    printf("OK RESETCONFIG\n");
    return 0;
}

/* Registers all console commands exposed by this task. */
static esp_err_t register_console_commands(void)
{
    const esp_console_cmd_t commands[] = {
        {
            .command = "setmotor",
            .help = "Set raw motor output: setmotor <motor_id> <pwm> <dir>",
            .hint = NULL,
            .func = &handle_setmotor,
        },
        {
            .command = "stopmotor",
            .help = "Stop raw motor output: stopmotor <motor_id>",
            .hint = NULL,
            .func = &handle_stopmotor,
        },
        {
            .command = "setposition",
            .help = "Set PID target position: setposition <motor_id> <position>",
            .hint = NULL,
            .func = &handle_setposition,
        },
        {
            .command = "getposition",
            .help = "Get current position: getposition <motor_id>",
            .hint = NULL,
            .func = &handle_getposition,
        },
        {
            .command = "setoffset",
            .help = "Set position offset: setoffset <motor_id> <offset>",
            .hint = NULL,
            .func = &handle_setoffset,
        },
        {
            .command = "setk",
            .help = "Set live PID gains: setk <motor_id> <kp> <ki> <kd>",
            .hint = NULL,
            .func = &handle_setk,
        },
        {
            .command = "getconfig",
            .help = "Read runtime config: getconfig [key]",
            .hint = NULL,
            .func = &handle_getconfig,
        },
        {
            .command = "setconfig",
            .help = "Set and save runtime config: setconfig <key> <value>",
            .hint = NULL,
            .func = &handle_setconfig,
        },
        {
            .command = "resetconfig",
            .help = "Reset runtime config defaults: resetconfig [key]",
            .hint = NULL,
            .func = &handle_resetconfig,
        },
    };

    /* Register a built-in help command before the project-specific commands. */
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_console_register_help_command());

    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        /* Stop registration on the first failure so init reports a clear error. */
        const esp_err_t err = esp_console_cmd_register(&commands[i]);
        if (err != ESP_OK) {
            return err;
        }
    }

    return ESP_OK;
}

/* Initializes console support; task creation stays in main.c. */
esp_err_t console_init(void)
{
    const esp_console_config_t console_config = {
        .max_cmdline_args = CONSOLE_MAX_ARGS,
        .max_cmdline_length = CONSOLE_MAX_LINE_LENGTH,
    };

    /* Initialize esp_console once, then attach this module's commands. */
    ESP_RETURN_ON_ERROR(esp_console_init(&console_config), "console", "initialize console");
    return register_console_commands();
}

/* Runs the console loop and executes registered commands from stdin. */
void console_task(void *arg)
{
    (void)arg;

    /* Print the boot-ready marker used during bring-up. */
    printf("READY %s\n", APP_AXIS_APP_NAME);
    linenoiseSetMultiLine(1);

    while (true) {
        int command_result = 0;
        char *line = linenoise("> ");

        if (line == NULL) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* Empty lines are ignored; non-empty lines go through esp_console. */
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
        }

        linenoiseFree(line);
    }
}
