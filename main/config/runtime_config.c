// Runtime config is one enum-indexed value table plus per-value NVS load/save helpers.

#include "config/runtime_config.h"

#include <stddef.h>

#include "nvs.h"

#define RUNTIME_CONFIG_NAMESPACE "rtcfg"

typedef struct {
    const char *nvs_key;
    int32_t default_value;
    int32_t value;
} runtime_config_entry_t;

static runtime_config_entry_t s_config[RUNTIME_CONFIG_COUNT] = {
    [RUNTIME_CONFIG_PID_KP_MILLI] = {
        .nvs_key = "pid_kp",
        .default_value = 500,
        .value = 500,
    },
    [RUNTIME_CONFIG_PID_KI_MILLI] = {
        .nvs_key = "pid_ki",
        .default_value = 0,
        .value = 0,
    },
    [RUNTIME_CONFIG_PID_KD_MILLI] = {
        .nvs_key = "pid_kd",
        .default_value = 50,
        .value = 50,
    },
    [RUNTIME_CONFIG_MAX_PWM] = {
        .nvs_key = "max_pwm",
        .default_value = 245,
        .value = 245,
    },
    [RUNTIME_CONFIG_MIN_START_PWM] = {
        .nvs_key = "min_pwm",
        .default_value = 200,
        .value = 200,
    },
    [RUNTIME_CONFIG_REFERENCE_SPEED_COUNTS_PER_SEC] = {
        .nvs_key = "ref_spd",
        .default_value = 800,
        .value = 800,
    },
    [RUNTIME_CONFIG_POSITIVE_SPEED_COUNTS_PER_SEC] = {
        .nvs_key = "pos_spd",
        .default_value = 1200,
        .value = 1200,
    },
    [RUNTIME_CONFIG_NEGATIVE_SPEED_COUNTS_PER_SEC] = {
        .nvs_key = "neg_spd",
        .default_value = 1200,
        .value = 1200,
    },
    [RUNTIME_CONFIG_SENSOR_SEEK_SPEED_COUNTS_PER_SEC] = {
        .nvs_key = "sensor_seek",
        .default_value = 600,
        .value = 600,
    },
    [RUNTIME_CONFIG_MAX_SPEED_COUNTS_PER_SEC] = {
        .nvs_key = "max_speed",
        .default_value = 20000,
        .value = 20000,
    },
    [RUNTIME_CONFIG_POSITION_TOLERANCE_COUNTS] = {
        .nvs_key = "pos_tol",
        .default_value = 20,
        .value = 20,
    },
    [RUNTIME_CONFIG_REFERENCE_TIMEOUT_MS] = {
        .nvs_key = "ref_to",
        .default_value = 30000,
        .value = 30000,
    },
    [RUNTIME_CONFIG_POSITIVE_TIMEOUT_MS] = {
        .nvs_key = "pos_to",
        .default_value = 30000,
        .value = 30000,
    },
    [RUNTIME_CONFIG_NEGATIVE_TIMEOUT_MS] = {
        .nvs_key = "neg_to",
        .default_value = 30000,
        .value = 30000,
    },
    [RUNTIME_CONFIG_STALL_CHECK_MS] = {
        .nvs_key = "stall_ms",
        .default_value = 500,
        .value = 500,
    },
    [RUNTIME_CONFIG_STALL_MIN_DELTA_COUNTS] = {
        .nvs_key = "stall_min",
        .default_value = 5,
        .value = 5,
    },
    [RUNTIME_CONFIG_DIRECTION_CHECK_DELAY_MS] = {
        .nvs_key = "dir_delay",
        .default_value = 100,
        .value = 100,
    },
    [RUNTIME_CONFIG_LIMIT_SWITCH_QUALIFY_MS] = {
        .nvs_key = "limit_sw",
        .default_value = 35,
        .value = 35,
    },
    [RUNTIME_CONFIG_MQTT_STATUS_PERIOD_MS] = {
        .nvs_key = "mqtt_ms",
        .default_value = 1000,
        .value = 1000,
    },
};

/* Verifies that an enum key has a populated metadata table entry. */
static esp_err_t check_key(runtime_config_key_t key)
{
    const int key_value = (int)key;
    if (key_value < 0 || key_value >= (int)RUNTIME_CONFIG_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_config[key].nvs_key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

/* Reads one runtime config value from the metadata table's RAM value. */
esp_err_t runtime_config_get(runtime_config_key_t key, int32_t *out_value)
{
    if (out_value == NULL || check_key(key) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_value = s_config[key].value;
    return ESP_OK;
}

/* Writes one runtime config value into the metadata table's RAM value. */
esp_err_t runtime_config_set(runtime_config_key_t key, int32_t value)
{
    if (check_key(key) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    s_config[key].value = value;
    return ESP_OK;
}

/* Restores one runtime config value to its table default. */
esp_err_t runtime_config_reset(runtime_config_key_t key)
{
    if (check_key(key) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    s_config[key].value = s_config[key].default_value;
    return ESP_OK;
}

/* Loads one runtime config value from NVS into RAM. */
esp_err_t runtime_config_load_nvs(runtime_config_key_t key)
{
    esp_err_t err = check_key(key);
    if (err != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    err = nvs_open(RUNTIME_CONFIG_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    /* NVS stores int32 values under the short key defined beside each config. */
    int32_t value = 0;
    err = nvs_get_i32(nvs, s_config[key].nvs_key, &value);
    nvs_close(nvs);
    if (err != ESP_OK) {
        return err;
    }

    s_config[key].value = value;
    return ESP_OK;
}

/* Stores one RAM runtime config value into NVS. */
esp_err_t runtime_config_store_nvs(runtime_config_key_t key)
{
    esp_err_t err = check_key(key);
    if (err != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    err = nvs_open(RUNTIME_CONFIG_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    /* Commit immediately so serial setconfig survives reset. */
    err = nvs_set_i32(nvs, s_config[key].nvs_key, s_config[key].value);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}
