// Runtime config is one RAM struct plus simple per-value NVS load/save helpers.

#include "config/runtime_config.h"

#include "nvs.h"

#define RUNTIME_CONFIG_NAMESPACE "rtcfg"

#define DEFAULT_PID_KP_MILLI 500
#define DEFAULT_PID_KI_MILLI 0
#define DEFAULT_PID_KD_MILLI 50
#define DEFAULT_MAX_PWM 245
#define DEFAULT_MIN_START_PWM 200
#define DEFAULT_REFERENCE_SPEED_COUNTS_PER_SEC 800
#define DEFAULT_POSITIVE_SPEED_COUNTS_PER_SEC 1200
#define DEFAULT_NEGATIVE_SPEED_COUNTS_PER_SEC 1200
#define DEFAULT_SENSOR_SEEK_SPEED_COUNTS_PER_SEC 600
#define DEFAULT_MAX_SPEED_COUNTS_PER_SEC 20000
#define DEFAULT_POSITION_TOLERANCE_COUNTS 20
#define DEFAULT_REFERENCE_TIMEOUT_MS 30000
#define DEFAULT_POSITIVE_TIMEOUT_MS 30000
#define DEFAULT_NEGATIVE_TIMEOUT_MS 30000
#define DEFAULT_STALL_CHECK_MS 500
#define DEFAULT_STALL_MIN_DELTA_COUNTS 5
#define DEFAULT_DIRECTION_CHECK_DELAY_MS 100
#define DEFAULT_LIMIT_SWITCH_QUALIFY_MS 35
#define DEFAULT_MQTT_STATUS_PERIOD_MS 1000

static runtime_config_t s_config;

static const char *const s_nvs_keys[RUNTIME_CONFIG_COUNT] = {
    [RUNTIME_CONFIG_PID_KP_MILLI] = "pid_kp",
    [RUNTIME_CONFIG_PID_KI_MILLI] = "pid_ki",
    [RUNTIME_CONFIG_PID_KD_MILLI] = "pid_kd",
    [RUNTIME_CONFIG_MAX_PWM] = "max_pwm",
    [RUNTIME_CONFIG_MIN_START_PWM] = "min_pwm",
    [RUNTIME_CONFIG_REFERENCE_SPEED_COUNTS_PER_SEC] = "ref_spd",
    [RUNTIME_CONFIG_POSITIVE_SPEED_COUNTS_PER_SEC] = "pos_spd",
    [RUNTIME_CONFIG_NEGATIVE_SPEED_COUNTS_PER_SEC] = "neg_spd",
    [RUNTIME_CONFIG_SENSOR_SEEK_SPEED_COUNTS_PER_SEC] = "sensor_seek",
    [RUNTIME_CONFIG_MAX_SPEED_COUNTS_PER_SEC] = "max_speed",
    [RUNTIME_CONFIG_POSITION_TOLERANCE_COUNTS] = "pos_tol",
    [RUNTIME_CONFIG_REFERENCE_TIMEOUT_MS] = "ref_to",
    [RUNTIME_CONFIG_POSITIVE_TIMEOUT_MS] = "pos_to",
    [RUNTIME_CONFIG_NEGATIVE_TIMEOUT_MS] = "neg_to",
    [RUNTIME_CONFIG_STALL_CHECK_MS] = "stall_ms",
    [RUNTIME_CONFIG_STALL_MIN_DELTA_COUNTS] = "stall_min",
    [RUNTIME_CONFIG_DIRECTION_CHECK_DELAY_MS] = "dir_delay",
    [RUNTIME_CONFIG_LIMIT_SWITCH_QUALIFY_MS] = "limit_sw",
    [RUNTIME_CONFIG_MQTT_STATUS_PERIOD_MS] = "mqtt_ms",
};

/* Verifies that an enum key is inside the runtime config table. */
static esp_err_t check_key(runtime_config_key_t key)
{
    const int key_value = (int) key;
    if (key_value < 0 || key_value >= (int) RUNTIME_CONFIG_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

/* Maps a runtime config key to its field inside the single RAM config struct. */
static esp_err_t value_ptr(runtime_config_key_t key, int32_t **out_value)
{
    if (out_value == NULL || check_key(key) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (key) {
    case RUNTIME_CONFIG_PID_KP_MILLI:
        *out_value = &s_config.pid_kp_milli;
        break;
    case RUNTIME_CONFIG_PID_KI_MILLI:
        *out_value = &s_config.pid_ki_milli;
        break;
    case RUNTIME_CONFIG_PID_KD_MILLI:
        *out_value = &s_config.pid_kd_milli;
        break;
    case RUNTIME_CONFIG_MAX_PWM:
        *out_value = &s_config.max_pwm;
        break;
    case RUNTIME_CONFIG_MIN_START_PWM:
        *out_value = &s_config.min_start_pwm;
        break;
    case RUNTIME_CONFIG_REFERENCE_SPEED_COUNTS_PER_SEC:
        *out_value = &s_config.reference_speed_counts_per_sec;
        break;
    case RUNTIME_CONFIG_POSITIVE_SPEED_COUNTS_PER_SEC:
        *out_value = &s_config.positive_speed_counts_per_sec;
        break;
    case RUNTIME_CONFIG_NEGATIVE_SPEED_COUNTS_PER_SEC:
        *out_value = &s_config.negative_speed_counts_per_sec;
        break;
    case RUNTIME_CONFIG_SENSOR_SEEK_SPEED_COUNTS_PER_SEC:
        *out_value = &s_config.sensor_seek_speed_counts_per_sec;
        break;
    case RUNTIME_CONFIG_MAX_SPEED_COUNTS_PER_SEC:
        *out_value = &s_config.max_speed_counts_per_sec;
        break;
    case RUNTIME_CONFIG_POSITION_TOLERANCE_COUNTS:
        *out_value = &s_config.position_tolerance_counts;
        break;
    case RUNTIME_CONFIG_REFERENCE_TIMEOUT_MS:
        *out_value = &s_config.reference_timeout_ms;
        break;
    case RUNTIME_CONFIG_POSITIVE_TIMEOUT_MS:
        *out_value = &s_config.positive_timeout_ms;
        break;
    case RUNTIME_CONFIG_NEGATIVE_TIMEOUT_MS:
        *out_value = &s_config.negative_timeout_ms;
        break;
    case RUNTIME_CONFIG_STALL_CHECK_MS:
        *out_value = &s_config.stall_check_ms;
        break;
    case RUNTIME_CONFIG_STALL_MIN_DELTA_COUNTS:
        *out_value = &s_config.stall_min_delta_counts;
        break;
    case RUNTIME_CONFIG_DIRECTION_CHECK_DELAY_MS:
        *out_value = &s_config.direction_check_delay_ms;
        break;
    case RUNTIME_CONFIG_LIMIT_SWITCH_QUALIFY_MS:
        *out_value = &s_config.limit_switch_qualify_ms;
        break;
    case RUNTIME_CONFIG_MQTT_STATUS_PERIOD_MS:
        *out_value = &s_config.mqtt_status_period_ms;
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

/* Returns the compile-time default value for one runtime config key. */
static esp_err_t default_value(runtime_config_key_t key, int32_t *out_value)
{
    if (out_value == NULL || check_key(key) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (key) {
    case RUNTIME_CONFIG_PID_KP_MILLI:
        *out_value = DEFAULT_PID_KP_MILLI;
        break;
    case RUNTIME_CONFIG_PID_KI_MILLI:
        *out_value = DEFAULT_PID_KI_MILLI;
        break;
    case RUNTIME_CONFIG_PID_KD_MILLI:
        *out_value = DEFAULT_PID_KD_MILLI;
        break;
    case RUNTIME_CONFIG_MAX_PWM:
        *out_value = DEFAULT_MAX_PWM;
        break;
    case RUNTIME_CONFIG_MIN_START_PWM:
        *out_value = DEFAULT_MIN_START_PWM;
        break;
    case RUNTIME_CONFIG_REFERENCE_SPEED_COUNTS_PER_SEC:
        *out_value = DEFAULT_REFERENCE_SPEED_COUNTS_PER_SEC;
        break;
    case RUNTIME_CONFIG_POSITIVE_SPEED_COUNTS_PER_SEC:
        *out_value = DEFAULT_POSITIVE_SPEED_COUNTS_PER_SEC;
        break;
    case RUNTIME_CONFIG_NEGATIVE_SPEED_COUNTS_PER_SEC:
        *out_value = DEFAULT_NEGATIVE_SPEED_COUNTS_PER_SEC;
        break;
    case RUNTIME_CONFIG_SENSOR_SEEK_SPEED_COUNTS_PER_SEC:
        *out_value = DEFAULT_SENSOR_SEEK_SPEED_COUNTS_PER_SEC;
        break;
    case RUNTIME_CONFIG_MAX_SPEED_COUNTS_PER_SEC:
        *out_value = DEFAULT_MAX_SPEED_COUNTS_PER_SEC;
        break;
    case RUNTIME_CONFIG_POSITION_TOLERANCE_COUNTS:
        *out_value = DEFAULT_POSITION_TOLERANCE_COUNTS;
        break;
    case RUNTIME_CONFIG_REFERENCE_TIMEOUT_MS:
        *out_value = DEFAULT_REFERENCE_TIMEOUT_MS;
        break;
    case RUNTIME_CONFIG_POSITIVE_TIMEOUT_MS:
        *out_value = DEFAULT_POSITIVE_TIMEOUT_MS;
        break;
    case RUNTIME_CONFIG_NEGATIVE_TIMEOUT_MS:
        *out_value = DEFAULT_NEGATIVE_TIMEOUT_MS;
        break;
    case RUNTIME_CONFIG_STALL_CHECK_MS:
        *out_value = DEFAULT_STALL_CHECK_MS;
        break;
    case RUNTIME_CONFIG_STALL_MIN_DELTA_COUNTS:
        *out_value = DEFAULT_STALL_MIN_DELTA_COUNTS;
        break;
    case RUNTIME_CONFIG_DIRECTION_CHECK_DELAY_MS:
        *out_value = DEFAULT_DIRECTION_CHECK_DELAY_MS;
        break;
    case RUNTIME_CONFIG_LIMIT_SWITCH_QUALIFY_MS:
        *out_value = DEFAULT_LIMIT_SWITCH_QUALIFY_MS;
        break;
    case RUNTIME_CONFIG_MQTT_STATUS_PERIOD_MS:
        *out_value = DEFAULT_MQTT_STATUS_PERIOD_MS;
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

/* Reads one runtime config value from the RAM config struct. */
esp_err_t runtime_config_get(runtime_config_key_t key, int32_t *out_value)
{
    int32_t *value = NULL;
    esp_err_t err = value_ptr(key, &value);
    if (err != ESP_OK) {
        return err;
    }

    if (out_value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_value = *value;
    return ESP_OK;
}

/* Writes one runtime config value into the RAM config struct. */
esp_err_t runtime_config_set(runtime_config_key_t key, int32_t value)
{
    int32_t *stored_value = NULL;
    esp_err_t err = value_ptr(key, &stored_value);
    if (err != ESP_OK) {
        return err;
    }

    *stored_value = value;
    return ESP_OK;
}

/* Loads one runtime config value from its compiled default. */
esp_err_t runtime_config_load_default(runtime_config_key_t key)
{
    int32_t value = 0;
    esp_err_t err = default_value(key, &value);
    if (err != ESP_OK) {
        return err;
    }

    return runtime_config_set(key, value);
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

    int32_t value = 0;
    err = nvs_get_i32(nvs, s_nvs_keys[key], &value);
    nvs_close(nvs);
    if (err != ESP_OK) {
        return err;
    }

    return runtime_config_set(key, value);
}

/* Stores one RAM runtime config value into NVS. */
esp_err_t runtime_config_store_nvs(runtime_config_key_t key)
{
    esp_err_t err = check_key(key);
    if (err != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    int32_t value = 0;
    err = runtime_config_get(key, &value);
    if (err != ESP_OK) {
        return err;
    }

    nvs_handle_t nvs;
    err = nvs_open(RUNTIME_CONFIG_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_i32(nvs, s_nvs_keys[key], value);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

/* Loads every runtime config value from compiled defaults. */
void runtime_config_load_defaults(void)
{
    for (runtime_config_key_t key = 0; key < RUNTIME_CONFIG_COUNT; ++key) {
        (void) runtime_config_load_default(key);
    }
}

/* Overlays every available NVS runtime config value onto RAM defaults. */
esp_err_t runtime_config_load_all_nvs(void)
{
    for (runtime_config_key_t key = 0; key < RUNTIME_CONFIG_COUNT; ++key) {
        esp_err_t err = runtime_config_load_nvs(key);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            continue;
        }
        if (err != ESP_OK) {
            return err;
        }
    }

    return ESP_OK;
}

/* Stores every RAM runtime config value into NVS. */
esp_err_t runtime_config_store_all_nvs(void)
{
    for (runtime_config_key_t key = 0; key < RUNTIME_CONFIG_COUNT; ++key) {
        esp_err_t err = runtime_config_store_nvs(key);
        if (err != ESP_OK) {
            return err;
        }
    }

    return ESP_OK;
}
