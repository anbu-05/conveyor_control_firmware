#include "runtime_config.h"

#include <string.h>

#include "app_state.h"
#include "config.h"
#include "esp_err.h"
#include "nvs.h"
#include "nvs_flash.h"

#define RUNTIME_CONFIG_NAMESPACE "conveyor_cfg"

typedef struct {
    int32_t run_pwm;
    int32_t run_speed_counts_per_sec;
    int32_t speed_kp_milli;
    int32_t speed_pwm_scale_milli;
    int32_t done_hold_ms;
    int32_t tx_detect_timeout_ms;
    int32_t tx_clear_timeout_ms;
    int32_t rx_detect_timeout_ms;
    int32_t rx_done_timeout_ms;
    int32_t mqtt_status_period_ms;
} runtime_config_t;

static runtime_config_t runtime_config;

static const runtime_config_t default_config = {
    .run_pwm = CONVEYOR_RUN_PWM,
    .run_speed_counts_per_sec = CONVEYOR_RUN_SPEED_COUNTS_PER_SEC,
    .speed_kp_milli = CONVEYOR_SPEED_KP_MILLI,
    .speed_pwm_scale_milli = CONVEYOR_SPEED_PWM_SCALE_MILLI,
    .done_hold_ms = CONVEYOR_DONE_HOLD_MS,
    .tx_detect_timeout_ms = CONVEYOR_TIMEOUT_TX_DETECT_MS,
    .tx_clear_timeout_ms = CONVEYOR_TIMEOUT_TX_CLEAR_MS,
    .rx_detect_timeout_ms = CONVEYOR_TIMEOUT_RX_DETECT_MS,
    .rx_done_timeout_ms = CONVEYOR_TIMEOUT_RX_DONE_MS,
    .mqtt_status_period_ms = CONVEYOR_MQTT_STATUS_PERIOD_MS,
};

static bool value_is_valid(const char *name, int32_t value)
{
    if (strcmp(name, "run_pwm") == 0) {
        return value >= 0 && value <= 255;
    }
    if (strcmp(name, "run_speed_counts_per_sec") == 0) {
        return value >= 0 && value <= 100000;
    }
    if (strcmp(name, "speed_kp_milli") == 0) {
        return value >= 0 && value <= 100000;
    }
    if (strcmp(name, "speed_pwm_scale_milli") == 0) {
        return value >= 0 && value <= 100000;
    }
    if (strcmp(name, "done_hold_ms") == 0) {
        return value >= 0 && value <= 60000;
    }
    if (strcmp(name, "tx_detect_timeout_ms") == 0) {
        return value >= 1 && value <= 600000;
    }
    if (strcmp(name, "tx_clear_timeout_ms") == 0) {
        return value >= 1 && value <= 600000;
    }
    if (strcmp(name, "rx_detect_timeout_ms") == 0) {
        return value >= 1 && value <= 600000;
    }
    if (strcmp(name, "rx_done_timeout_ms") == 0) {
        return value >= 1 && value <= 600000;
    }
    if (strcmp(name, "mqtt_status_period_ms") == 0) {
        return value >= 100 && value <= 60000;
    }

    return false;
}

static const char *storage_key(const char *name)
{
    if (strcmp(name, "run_pwm") == 0) {
        return "run_pwm";
    }
    if (strcmp(name, "run_speed_counts_per_sec") == 0) {
        return "run_speed";
    }
    if (strcmp(name, "speed_kp_milli") == 0) {
        return "speed_kp";
    }
    if (strcmp(name, "speed_pwm_scale_milli") == 0) {
        return "speed_scale";
    }
    if (strcmp(name, "done_hold_ms") == 0) {
        return "done_hold_ms";
    }
    if (strcmp(name, "tx_detect_timeout_ms") == 0) {
        return "tx_det_ms";
    }
    if (strcmp(name, "tx_clear_timeout_ms") == 0) {
        return "tx_clr_ms";
    }
    if (strcmp(name, "rx_detect_timeout_ms") == 0) {
        return "rx_det_ms";
    }
    if (strcmp(name, "rx_done_timeout_ms") == 0) {
        return "rx_done_ms";
    }
    if (strcmp(name, "mqtt_status_period_ms") == 0) {
        return "mqtt_stat_ms";
    }

    return NULL;
}

bool runtime_config_value_is_valid(const char *name, int32_t value)
{
    return value_is_valid(name, value);
}

static bool set_ram_value(const char *name, int32_t value)
{
    if (!value_is_valid(name, value)) {
        return false;
    }

    if (strcmp(name, "run_pwm") == 0) {
        runtime_config.run_pwm = value;
        return true;
    }
    if (strcmp(name, "run_speed_counts_per_sec") == 0) {
        runtime_config.run_speed_counts_per_sec = value;
        return true;
    }
    if (strcmp(name, "speed_kp_milli") == 0) {
        runtime_config.speed_kp_milli = value;
        return true;
    }
    if (strcmp(name, "speed_pwm_scale_milli") == 0) {
        runtime_config.speed_pwm_scale_milli = value;
        return true;
    }
    if (strcmp(name, "done_hold_ms") == 0) {
        runtime_config.done_hold_ms = value;
        return true;
    }
    if (strcmp(name, "tx_detect_timeout_ms") == 0) {
        runtime_config.tx_detect_timeout_ms = value;
        return true;
    }
    if (strcmp(name, "tx_clear_timeout_ms") == 0) {
        runtime_config.tx_clear_timeout_ms = value;
        return true;
    }
    if (strcmp(name, "rx_detect_timeout_ms") == 0) {
        runtime_config.rx_detect_timeout_ms = value;
        return true;
    }
    if (strcmp(name, "rx_done_timeout_ms") == 0) {
        runtime_config.rx_done_timeout_ms = value;
        return true;
    }
    if (strcmp(name, "mqtt_status_period_ms") == 0) {
        runtime_config.mqtt_status_period_ms = value;
        return true;
    }

    return false;
}

bool runtime_config_get_value(const char *name, int32_t *value)
{
    if (name == NULL || value == NULL) {
        return false;
    }

    if (strcmp(name, "run_pwm") == 0) {
        *value = runtime_config.run_pwm;
        return true;
    }
    if (strcmp(name, "run_speed_counts_per_sec") == 0) {
        *value = runtime_config.run_speed_counts_per_sec;
        return true;
    }
    if (strcmp(name, "speed_kp_milli") == 0) {
        *value = runtime_config.speed_kp_milli;
        return true;
    }
    if (strcmp(name, "speed_pwm_scale_milli") == 0) {
        *value = runtime_config.speed_pwm_scale_milli;
        return true;
    }
    if (strcmp(name, "done_hold_ms") == 0) {
        *value = runtime_config.done_hold_ms;
        return true;
    }
    if (strcmp(name, "tx_detect_timeout_ms") == 0) {
        *value = runtime_config.tx_detect_timeout_ms;
        return true;
    }
    if (strcmp(name, "tx_clear_timeout_ms") == 0) {
        *value = runtime_config.tx_clear_timeout_ms;
        return true;
    }
    if (strcmp(name, "rx_detect_timeout_ms") == 0) {
        *value = runtime_config.rx_detect_timeout_ms;
        return true;
    }
    if (strcmp(name, "rx_done_timeout_ms") == 0) {
        *value = runtime_config.rx_done_timeout_ms;
        return true;
    }
    if (strcmp(name, "mqtt_status_period_ms") == 0) {
        *value = runtime_config.mqtt_status_period_ms;
        return true;
    }

    return false;
}

static bool save_one_value(const char *name, int32_t value)
{
    nvs_handle_t handle;
    esp_err_t err;
    const char *key = storage_key(name);

    if (key == NULL) {
        return false;
    }

    err = nvs_open(RUNTIME_CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return false;
    }

    err = nvs_set_i32(handle, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err == ESP_OK;
}

bool runtime_config_set_value(const char *name, int32_t value)
{
    if (name == NULL || !value_is_valid(name, value)) {
        return false;
    }

    if (!save_one_value(name, value)) {
        return false;
    }

    return set_ram_value(name, value);
}

static bool save_all_defaults(void)
{
    nvs_handle_t handle;
    esp_err_t err;

    err = nvs_open(RUNTIME_CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return false;
    }

    err = nvs_set_i32(handle, storage_key("run_pwm"), default_config.run_pwm);
    if (err == ESP_OK) {
        err = nvs_set_i32(handle, storage_key("run_speed_counts_per_sec"), default_config.run_speed_counts_per_sec);
    }
    if (err == ESP_OK) {
        err = nvs_set_i32(handle, storage_key("speed_kp_milli"), default_config.speed_kp_milli);
    }
    if (err == ESP_OK) {
        err = nvs_set_i32(handle, storage_key("speed_pwm_scale_milli"), default_config.speed_pwm_scale_milli);
    }
    if (err == ESP_OK) {
        err = nvs_set_i32(handle, storage_key("done_hold_ms"), default_config.done_hold_ms);
    }
    if (err == ESP_OK) {
        err = nvs_set_i32(handle, storage_key("tx_detect_timeout_ms"), default_config.tx_detect_timeout_ms);
    }
    if (err == ESP_OK) {
        err = nvs_set_i32(handle, storage_key("tx_clear_timeout_ms"), default_config.tx_clear_timeout_ms);
    }
    if (err == ESP_OK) {
        err = nvs_set_i32(handle, storage_key("rx_detect_timeout_ms"), default_config.rx_detect_timeout_ms);
    }
    if (err == ESP_OK) {
        err = nvs_set_i32(handle, storage_key("rx_done_timeout_ms"), default_config.rx_done_timeout_ms);
    }
    if (err == ESP_OK) {
        err = nvs_set_i32(handle, storage_key("mqtt_status_period_ms"), default_config.mqtt_status_period_ms);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err == ESP_OK;
}

bool runtime_config_reset_defaults(void)
{
    if (!save_all_defaults()) {
        return false;
    }

    runtime_config = default_config;
    return true;
}

static void load_one_value(nvs_handle_t handle, const char *name)
{
    int32_t value = 0;
    const char *key = storage_key(name);

    if (key != NULL && nvs_get_i32(handle, key, &value) == ESP_OK) {
        (void)set_ram_value(name, value);
    }
}

static void load_saved_values(void)
{
    nvs_handle_t handle;

    if (nvs_open(RUNTIME_CONFIG_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }

    load_one_value(handle, "run_pwm");
    load_one_value(handle, "run_speed_counts_per_sec");
    load_one_value(handle, "speed_kp_milli");
    load_one_value(handle, "speed_pwm_scale_milli");
    load_one_value(handle, "done_hold_ms");
    load_one_value(handle, "tx_detect_timeout_ms");
    load_one_value(handle, "tx_clear_timeout_ms");
    load_one_value(handle, "rx_detect_timeout_ms");
    load_one_value(handle, "rx_done_timeout_ms");
    load_one_value(handle, "mqtt_status_period_ms");

    nvs_close(handle);
}

void configure_runtime_config(void)
{
    esp_err_t err;

    runtime_config = default_config;

    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }

    load_saved_values();
}

void runtime_config_print_all(void)
{
    console_printf("CONFIG run_pwm %ld\r\n", (long)runtime_config.run_pwm);
    console_printf("CONFIG run_speed_counts_per_sec %ld\r\n", (long)runtime_config.run_speed_counts_per_sec);
    console_printf("CONFIG speed_kp %ld.%03ld\r\n",
                   (long)(runtime_config.speed_kp_milli / 1000),
                   (long)(runtime_config.speed_kp_milli % 1000));
    console_printf("CONFIG speed_pwm_scale_milli %ld\r\n", (long)runtime_config.speed_pwm_scale_milli);
    console_printf("CONFIG done_hold_ms %ld\r\n", (long)runtime_config.done_hold_ms);
    console_printf("CONFIG tx_detect_timeout_ms %ld\r\n", (long)runtime_config.tx_detect_timeout_ms);
    console_printf("CONFIG tx_clear_timeout_ms %ld\r\n", (long)runtime_config.tx_clear_timeout_ms);
    console_printf("CONFIG rx_detect_timeout_ms %ld\r\n", (long)runtime_config.rx_detect_timeout_ms);
    console_printf("CONFIG rx_done_timeout_ms %ld\r\n", (long)runtime_config.rx_done_timeout_ms);
    console_printf("CONFIG mqtt_status_period_ms %ld\r\n", (long)runtime_config.mqtt_status_period_ms);
}

int runtime_config_run_pwm(void)
{
    return runtime_config.run_pwm;
}

int runtime_config_run_speed_counts_per_sec(void)
{
    return runtime_config.run_speed_counts_per_sec;
}

int runtime_config_speed_kp_milli(void)
{
    return runtime_config.speed_kp_milli;
}

int runtime_config_speed_pwm_scale_milli(void)
{
    return runtime_config.speed_pwm_scale_milli;
}

bool runtime_config_set_speed_kp_milli(int32_t value)
{
    return runtime_config_set_value("speed_kp_milli", value);
}

uint32_t runtime_config_done_hold_ms(void)
{
    return (uint32_t)runtime_config.done_hold_ms;
}

uint32_t runtime_config_tx_detect_timeout_ms(void)
{
    return (uint32_t)runtime_config.tx_detect_timeout_ms;
}

uint32_t runtime_config_tx_clear_timeout_ms(void)
{
    return (uint32_t)runtime_config.tx_clear_timeout_ms;
}

uint32_t runtime_config_rx_detect_timeout_ms(void)
{
    return (uint32_t)runtime_config.rx_detect_timeout_ms;
}

uint32_t runtime_config_rx_done_timeout_ms(void)
{
    return (uint32_t)runtime_config.rx_done_timeout_ms;
}

uint32_t runtime_config_mqtt_status_period_ms(void)
{
    return (uint32_t)runtime_config.mqtt_status_period_ms;
}
