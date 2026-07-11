// Runtime config is one enum-indexed value table. Values start from flash defaults and can change in RAM.

#include "config/runtime_config.h"

#include <stddef.h>

typedef struct {
    int32_t default_value;
    int32_t value;
} runtime_config_entry_t;

static runtime_config_entry_t s_config[RUNTIME_CONFIG_COUNT] = {
    /* Upper PWM duty clamp used by raw setmotor and PID motor output. */
    [RUNTIME_CONFIG_MAX_PWM] = {
        .default_value = 255,
        .value = 255,
    },
    /* Position error deadband applied before PID terms. */
    [RUNTIME_CONFIG_POSITION_TOLERANCE_COUNTS] = {
        .default_value = 20,
        .value = 20,
    },
};

/* Verifies that an enum key maps into the config table. */
static esp_err_t check_key(runtime_config_key_t key)
{
    const int key_value = (int)key;
    if (key_value < 0 || key_value >= (int)RUNTIME_CONFIG_COUNT) {
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
