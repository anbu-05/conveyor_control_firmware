#include "runtime_config.h"

#include <stdio.h>
#include <string.h>

#include "app_state.h"
#include "config.h"
#include "esp_err.h"
#include "nvs.h"
#include "nvs_flash.h"

#define RUNTIME_CONFIG_NAMESPACE "conveyor_cfg" /* NVS namespace for saved runtime config values. */
#define RUNTIME_CONFIG_WIFI_SSID_MAX 32
#define RUNTIME_CONFIG_WIFI_PASS_MAX 64
#define RUNTIME_CONFIG_CONVEYOR_ID_MAX 32
#define RUNTIME_CONFIG_MQTT_URI_MAX 128
#define RUNTIME_CONFIG_MQTT_TOPIC_MAX 96

typedef struct {
    int32_t run_pwm;                    /* Direct-PWM debug value. Runtime key: run_pwm. */
    int32_t run_speed_counts_per_sec;   /* Job speed target in encoder counts/sec. */
    int32_t speed_kp_milli;             /* Speed P gain scaled by 1000; 50 means 0.050. */
    int32_t speed_kd_milli;             /* Speed D gain scaled by 1000; 0 disables D control. */
    int32_t done_hold_ms;               /* DONE-state hold time before returning to IDLE. */
    int32_t tx_detect_timeout_ms;       /* TX timeout while waiting for tray detect at S1. */
    int32_t tx_clear_timeout_ms;        /* TX timeout while waiting for tray to clear S1. */
    int32_t rx_detect_timeout_ms;       /* RX timeout while waiting for tray detect at S0. */
    int32_t rx_done_timeout_ms;         /* RX timeout while waiting for tray detect at S1. */
    int32_t mqtt_status_period_ms;      /* Period between MQTT status publishes. */
    char wifi_ssid[RUNTIME_CONFIG_WIFI_SSID_MAX];
    char wifi_pass[RUNTIME_CONFIG_WIFI_PASS_MAX];
    char conveyor_id[RUNTIME_CONFIG_CONVEYOR_ID_MAX];
    char mqtt_broker_uri[RUNTIME_CONFIG_MQTT_URI_MAX];
    char mqtt_topic_cmd[RUNTIME_CONFIG_MQTT_TOPIC_MAX];
    char mqtt_topic_emergency[RUNTIME_CONFIG_MQTT_TOPIC_MAX];
    char mqtt_topic_feedback[RUNTIME_CONFIG_MQTT_TOPIC_MAX];
    char mqtt_topic_all_emergency[RUNTIME_CONFIG_MQTT_TOPIC_MAX];
    char mqtt_topic_tray[RUNTIME_CONFIG_MQTT_TOPIC_MAX];
} runtime_config_t;

static runtime_config_t runtime_config;

/*
 * Compile-time defaults come from config.h. Saved NVS values override these
 * during boot, and serial setconfig writes updated values back to NVS.
 */
static const runtime_config_t default_config = {
    .run_pwm = CONVEYOR_RUN_PWM,
    .run_speed_counts_per_sec = CONVEYOR_RUN_SPEED_COUNTS_PER_SEC,
    .speed_kp_milli = CONVEYOR_SPEED_KP_MILLI,
    .speed_kd_milli = CONVEYOR_SPEED_KD_MILLI,
    .done_hold_ms = CONVEYOR_DONE_HOLD_MS,
    .tx_detect_timeout_ms = CONVEYOR_TIMEOUT_TX_DETECT_MS,
    .tx_clear_timeout_ms = CONVEYOR_TIMEOUT_TX_CLEAR_MS,
    .rx_detect_timeout_ms = CONVEYOR_TIMEOUT_RX_DETECT_MS,
    .rx_done_timeout_ms = CONVEYOR_TIMEOUT_RX_DONE_MS,
    .mqtt_status_period_ms = CONVEYOR_MQTT_STATUS_PERIOD_MS,
    .wifi_ssid = CONVEYOR_WIFI_SSID,
    .wifi_pass = CONVEYOR_WIFI_PASS,
    .conveyor_id = CONVEYOR_ID,
    .mqtt_broker_uri = CONVEYOR_MQTT_BROKER_URI,
    .mqtt_topic_cmd = CONVEYOR_MQTT_TOPIC_CMD,
    .mqtt_topic_emergency = CONVEYOR_MQTT_TOPIC_EMERGENCY,
    .mqtt_topic_feedback = CONVEYOR_MQTT_TOPIC_FEEDBACK,
    .mqtt_topic_all_emergency = CONVEYOR_MQTT_TOPIC_ALL_EMERGENCY,
    .mqtt_topic_tray = CONVEYOR_MQTT_TOPIC_TRAY,
};

static bool value_is_valid(const char *name, int32_t value)
{
    if (strcmp(name, "run_pwm") == 0) {
        return value >= 0 && value <= 255; /* 8-bit LEDC duty. */
    }
    if (strcmp(name, "run_speed_counts_per_sec") == 0) {
        return value >= 0 && value <= 100000; /* Positive job speed only. */
    }
    if (strcmp(name, "speed_kp_milli") == 0) {
        return value >= 0 && value <= 100000; /* 0.000 to 100.000. */
    }
    if (strcmp(name, "speed_kd_milli") == 0) {
        return value >= 0 && value <= 100000; /* 0.000 to 100.000. */
    }
    if (strcmp(name, "done_hold_ms") == 0) {
        return value >= 0 && value <= 60000; /* 0 allows immediate IDLE return. */
    }
    if (strcmp(name, "tx_detect_timeout_ms") == 0) {
        return value >= 1 && value <= 600000; /* 1 ms to 10 minutes. */
    }
    if (strcmp(name, "tx_clear_timeout_ms") == 0) {
        return value >= 1 && value <= 600000; /* 1 ms to 10 minutes. */
    }
    if (strcmp(name, "rx_detect_timeout_ms") == 0) {
        return value >= 1 && value <= 600000; /* 1 ms to 10 minutes. */
    }
    if (strcmp(name, "rx_done_timeout_ms") == 0) {
        return value >= 1 && value <= 600000; /* 1 ms to 10 minutes. */
    }
    if (strcmp(name, "mqtt_status_period_ms") == 0) {
        return value >= 100 && value <= 60000; /* Avoid spamming MQTT faster than 10 Hz. */
    }

    return false;
}

static size_t string_value_max_len(const char *name)
{
    if (strcmp(name, "wifi_ssid") == 0) {
        return RUNTIME_CONFIG_WIFI_SSID_MAX;
    }
    if (strcmp(name, "wifi_pass") == 0) {
        return RUNTIME_CONFIG_WIFI_PASS_MAX;
    }
    if (strcmp(name, "conveyor_id") == 0) {
        return RUNTIME_CONFIG_CONVEYOR_ID_MAX;
    }
    if (strcmp(name, "mqtt_broker_uri") == 0) {
        return RUNTIME_CONFIG_MQTT_URI_MAX;
    }
    if (strcmp(name, "mqtt_topic_cmd") == 0 ||
        strcmp(name, "mqtt_topic_emergency") == 0 ||
        strcmp(name, "mqtt_topic_feedback") == 0 ||
        strcmp(name, "mqtt_topic_all_emergency") == 0 ||
        strcmp(name, "mqtt_topic_tray") == 0) {
        return RUNTIME_CONFIG_MQTT_TOPIC_MAX;
    }

    return 0;
}

static bool string_is_valid(const char *name, const char *value)
{
    size_t max_len = 0;
    size_t len = 0;

    if (name == NULL || value == NULL || value[0] == '\0') {
        return false;
    }

    max_len = string_value_max_len(name);
    if (max_len == 0) {
        return false;
    }

    len = strlen(value);
    return len > 0 && len < max_len;
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
    if (strcmp(name, "speed_kd_milli") == 0) {
        return "speed_kd";
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

static const char *string_storage_key(const char *name)
{
    if (strcmp(name, "wifi_ssid") == 0) {
        return "wifi_ssid";
    }
    if (strcmp(name, "wifi_pass") == 0) {
        return "wifi_pass";
    }
    if (strcmp(name, "conveyor_id") == 0) {
        return "conveyor_id";
    }
    if (strcmp(name, "mqtt_broker_uri") == 0) {
        return "mqtt_broker";
    }
    if (strcmp(name, "mqtt_topic_cmd") == 0) {
        return "mqtt_cmd";
    }
    if (strcmp(name, "mqtt_topic_emergency") == 0) {
        return "mqtt_emerg";
    }
    if (strcmp(name, "mqtt_topic_feedback") == 0) {
        return "mqtt_feed";
    }
    if (strcmp(name, "mqtt_topic_all_emergency") == 0) {
        return "mqtt_all_emg";
    }
    if (strcmp(name, "mqtt_topic_tray") == 0) {
        return "mqtt_tray";
    }

    return NULL;
}

bool runtime_config_value_is_valid(const char *name, int32_t value)
{
    return value_is_valid(name, value);
}

bool runtime_config_string_is_valid(const char *name, const char *value)
{
    return string_is_valid(name, value);
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
    if (strcmp(name, "speed_kd_milli") == 0) {
        runtime_config.speed_kd_milli = value;
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

static bool set_ram_string(const char *name, const char *value)
{
    if (!string_is_valid(name, value)) {
        return false;
    }

    if (strcmp(name, "wifi_ssid") == 0) {
        snprintf(runtime_config.wifi_ssid, sizeof(runtime_config.wifi_ssid), "%s", value);
        return true;
    }
    if (strcmp(name, "wifi_pass") == 0) {
        snprintf(runtime_config.wifi_pass, sizeof(runtime_config.wifi_pass), "%s", value);
        return true;
    }
    if (strcmp(name, "conveyor_id") == 0) {
        snprintf(runtime_config.conveyor_id, sizeof(runtime_config.conveyor_id), "%s", value);
        return true;
    }
    if (strcmp(name, "mqtt_broker_uri") == 0) {
        snprintf(runtime_config.mqtt_broker_uri, sizeof(runtime_config.mqtt_broker_uri), "%s", value);
        return true;
    }
    if (strcmp(name, "mqtt_topic_cmd") == 0) {
        snprintf(runtime_config.mqtt_topic_cmd, sizeof(runtime_config.mqtt_topic_cmd), "%s", value);
        return true;
    }
    if (strcmp(name, "mqtt_topic_emergency") == 0) {
        snprintf(runtime_config.mqtt_topic_emergency, sizeof(runtime_config.mqtt_topic_emergency), "%s", value);
        return true;
    }
    if (strcmp(name, "mqtt_topic_feedback") == 0) {
        snprintf(runtime_config.mqtt_topic_feedback, sizeof(runtime_config.mqtt_topic_feedback), "%s", value);
        return true;
    }
    if (strcmp(name, "mqtt_topic_all_emergency") == 0) {
        snprintf(runtime_config.mqtt_topic_all_emergency, sizeof(runtime_config.mqtt_topic_all_emergency), "%s", value);
        return true;
    }
    if (strcmp(name, "mqtt_topic_tray") == 0) {
        snprintf(runtime_config.mqtt_topic_tray, sizeof(runtime_config.mqtt_topic_tray), "%s", value);
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
    if (strcmp(name, "speed_kd_milli") == 0) {
        *value = runtime_config.speed_kd_milli;
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

bool runtime_config_get_string(const char *name, const char **value)
{
    if (name == NULL || value == NULL) {
        return false;
    }

    if (strcmp(name, "wifi_ssid") == 0) {
        *value = runtime_config.wifi_ssid;
        return true;
    }
    if (strcmp(name, "wifi_pass") == 0) {
        *value = runtime_config.wifi_pass;
        return true;
    }
    if (strcmp(name, "conveyor_id") == 0) {
        *value = runtime_config.conveyor_id;
        return true;
    }
    if (strcmp(name, "mqtt_broker_uri") == 0) {
        *value = runtime_config.mqtt_broker_uri;
        return true;
    }
    if (strcmp(name, "mqtt_topic_cmd") == 0) {
        *value = runtime_config.mqtt_topic_cmd;
        return true;
    }
    if (strcmp(name, "mqtt_topic_emergency") == 0) {
        *value = runtime_config.mqtt_topic_emergency;
        return true;
    }
    if (strcmp(name, "mqtt_topic_feedback") == 0) {
        *value = runtime_config.mqtt_topic_feedback;
        return true;
    }
    if (strcmp(name, "mqtt_topic_all_emergency") == 0) {
        *value = runtime_config.mqtt_topic_all_emergency;
        return true;
    }
    if (strcmp(name, "mqtt_topic_tray") == 0) {
        *value = runtime_config.mqtt_topic_tray;
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

static bool save_one_string(const char *name, const char *value)
{
    nvs_handle_t handle;
    esp_err_t err;
    const char *key = string_storage_key(name);

    if (key == NULL || !string_is_valid(name, value)) {
        return false;
    }

    err = nvs_open(RUNTIME_CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return false;
    }

    err = nvs_set_str(handle, key, value);
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

bool runtime_config_set_string(const char *name, const char *value)
{
    if (name == NULL || !string_is_valid(name, value)) {
        return false;
    }

    if (!save_one_string(name, value)) {
        return false;
    }

    return set_ram_string(name, value);
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
        err = nvs_set_i32(handle, storage_key("speed_kd_milli"), default_config.speed_kd_milli);
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
        err = nvs_set_str(handle, string_storage_key("wifi_ssid"), default_config.wifi_ssid);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, string_storage_key("wifi_pass"), default_config.wifi_pass);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, string_storage_key("conveyor_id"), default_config.conveyor_id);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, string_storage_key("mqtt_broker_uri"), default_config.mqtt_broker_uri);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, string_storage_key("mqtt_topic_cmd"), default_config.mqtt_topic_cmd);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, string_storage_key("mqtt_topic_emergency"), default_config.mqtt_topic_emergency);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, string_storage_key("mqtt_topic_feedback"), default_config.mqtt_topic_feedback);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, string_storage_key("mqtt_topic_all_emergency"), default_config.mqtt_topic_all_emergency);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, string_storage_key("mqtt_topic_tray"), default_config.mqtt_topic_tray);
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

static void load_one_string(nvs_handle_t handle, const char *name)
{
    const char *key = string_storage_key(name);
    char value[RUNTIME_CONFIG_MQTT_URI_MAX];
    size_t length = sizeof(value);

    if (key == NULL) {
        return;
    }

    if (strcmp(name, "wifi_ssid") == 0) {
        length = RUNTIME_CONFIG_WIFI_SSID_MAX;
    } else if (strcmp(name, "wifi_pass") == 0) {
        length = RUNTIME_CONFIG_WIFI_PASS_MAX;
    } else if (strcmp(name, "conveyor_id") == 0) {
        length = RUNTIME_CONFIG_CONVEYOR_ID_MAX;
    } else if (strcmp(name, "mqtt_broker_uri") == 0) {
        length = RUNTIME_CONFIG_MQTT_URI_MAX;
    } else {
        length = RUNTIME_CONFIG_MQTT_TOPIC_MAX;
    }

    if (nvs_get_str(handle, key, value, &length) == ESP_OK) {
        (void)set_ram_string(name, value);
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
    load_one_value(handle, "speed_kd_milli");
    load_one_value(handle, "done_hold_ms");
    load_one_value(handle, "tx_detect_timeout_ms");
    load_one_value(handle, "tx_clear_timeout_ms");
    load_one_value(handle, "rx_detect_timeout_ms");
    load_one_value(handle, "rx_done_timeout_ms");
    load_one_value(handle, "mqtt_status_period_ms");
    load_one_string(handle, "wifi_ssid");
    load_one_string(handle, "wifi_pass");
    load_one_string(handle, "conveyor_id");
    load_one_string(handle, "mqtt_broker_uri");
    load_one_string(handle, "mqtt_topic_cmd");
    load_one_string(handle, "mqtt_topic_emergency");
    load_one_string(handle, "mqtt_topic_feedback");
    load_one_string(handle, "mqtt_topic_all_emergency");
    load_one_string(handle, "mqtt_topic_tray");

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
    console_printf("CONFIG speed_kd %ld.%03ld\r\n",
                   (long)(runtime_config.speed_kd_milli / 1000),
                   (long)(runtime_config.speed_kd_milli % 1000));
    console_printf("CONFIG done_hold_ms %ld\r\n", (long)runtime_config.done_hold_ms);
    console_printf("CONFIG tx_detect_timeout_ms %ld\r\n", (long)runtime_config.tx_detect_timeout_ms);
    console_printf("CONFIG tx_clear_timeout_ms %ld\r\n", (long)runtime_config.tx_clear_timeout_ms);
    console_printf("CONFIG rx_detect_timeout_ms %ld\r\n", (long)runtime_config.rx_detect_timeout_ms);
    console_printf("CONFIG rx_done_timeout_ms %ld\r\n", (long)runtime_config.rx_done_timeout_ms);
    console_printf("CONFIG mqtt_status_period_ms %ld\r\n", (long)runtime_config.mqtt_status_period_ms);
    console_printf("CONFIG wifi_ssid %s\r\n", runtime_config.wifi_ssid);
    console_printf("CONFIG wifi_pass %s\r\n", runtime_config.wifi_pass);
    console_printf("CONFIG conveyor_id %s\r\n", runtime_config.conveyor_id);
    console_printf("CONFIG mqtt_broker_uri %s\r\n", runtime_config.mqtt_broker_uri);
    console_printf("CONFIG mqtt_topic_cmd %s\r\n", runtime_config.mqtt_topic_cmd);
    console_printf("CONFIG mqtt_topic_emergency %s\r\n", runtime_config.mqtt_topic_emergency);
    console_printf("CONFIG mqtt_topic_feedback %s\r\n", runtime_config.mqtt_topic_feedback);
    console_printf("CONFIG mqtt_topic_all_emergency %s\r\n", runtime_config.mqtt_topic_all_emergency);
    console_printf("CONFIG mqtt_topic_tray %s\r\n", runtime_config.mqtt_topic_tray);
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

int runtime_config_speed_kd_milli(void)
{
    return runtime_config.speed_kd_milli;
}

bool runtime_config_set_speed_kp_milli(int32_t value)
{
    return runtime_config_set_value("speed_kp_milli", value);
}

bool runtime_config_set_speed_kd_milli(int32_t value)
{
    return runtime_config_set_value("speed_kd_milli", value);
}

bool runtime_config_reset_speed_gains(void)
{
    if (!runtime_config_set_speed_kp_milli(default_config.speed_kp_milli)) {
        return false;
    }

    if (!runtime_config_set_speed_kd_milli(default_config.speed_kd_milli)) {
        return false;
    }

    return true;
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

const char *runtime_config_wifi_ssid(void)
{
    return runtime_config.wifi_ssid;
}

const char *runtime_config_wifi_pass(void)
{
    return runtime_config.wifi_pass;
}

const char *runtime_config_conveyor_id(void)
{
    return runtime_config.conveyor_id;
}

const char *runtime_config_mqtt_broker_uri(void)
{
    return runtime_config.mqtt_broker_uri;
}

const char *runtime_config_mqtt_topic_cmd(void)
{
    return runtime_config.mqtt_topic_cmd;
}

const char *runtime_config_mqtt_topic_emergency(void)
{
    return runtime_config.mqtt_topic_emergency;
}

const char *runtime_config_mqtt_topic_feedback(void)
{
    return runtime_config.mqtt_topic_feedback;
}

const char *runtime_config_mqtt_topic_all_emergency(void)
{
    return runtime_config.mqtt_topic_all_emergency;
}

const char *runtime_config_mqtt_topic_tray(void)
{
    return runtime_config.mqtt_topic_tray;
}
