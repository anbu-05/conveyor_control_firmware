#include "mqtt_task.h"

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "runtime_config.h"

static const char *TAG = "ConveyorMQTT";

static esp_mqtt_client_handle_t mqtt_client;
static volatile bool mqtt_connected;
static bool tray_status_published;
static bool last_has_tray;
static bool central_status_published = true;
static conveyor_state_t last_central_state;

typedef struct {
    bool active;
    conveyor_cmd_type_t type;
    char command_id[64];
} central_command_tracker_t;

static central_command_tracker_t central_command;

bool mqtt_task_is_connected(void)
{
    return mqtt_connected;
}

static void publish_to_topic(const char *topic, const char *text)
{
    if (!mqtt_connected || mqtt_client == NULL || topic == NULL || text == NULL) {
        return;
    }

    esp_mqtt_client_publish(mqtt_client, topic, text, 0, 0, 0);
}

static void publish_text(const char *text)
{
    publish_to_topic(CONVEYOR_MQTT_TOPIC_FEEDBACK, text);
}

static bool is_safe_json_text(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return false;
    }

    for (size_t i = 0; text[i] != '\0'; i++) {
        char ch = text[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' ||
              ch == '.' || ch == ':' || ch == '/')) {
            return false;
        }
    }

    return true;
}

static void publish_central_result(const char *command_id, const char *status, const char *detail)
{
    char message[CONVEYOR_MQTT_PAYLOAD_MAX];

    if (command_id == NULL) {
        command_id = "";
    }
    if (status == NULL) {
        status = "failure";
    }
    if (detail == NULL) {
        detail = "";
    }

    snprintf(message,
             sizeof(message),
             "{\"command_id\":\"%s\",\"command_status\":\"%s\",\"message\":\"%s\"}",
             command_id,
             status,
             detail);
    publish_to_topic(CONVEYOR_MQTT_TOPIC_CENTRAL_RESULT, message);
}

static void publish_central_command_list(const char *command_id)
{
    char message[CONVEYOR_MQTT_PAYLOAD_MAX];

    snprintf(message,
             sizeof(message),
             "{\"command_id\":\"%s\",\"command_status\":\"success\",\"message\":\"supported commands\",\"commands\":[\"transmit\",\"receive\",\"stop\",\"clear_error\",\"get_commands\"]}",
             command_id);
    publish_to_topic(CONVEYOR_MQTT_TOPIC_CENTRAL_RESULT, message);
}

static void publish_central_status(const conveyor_status_t *status, bool force)
{
    conveyor_tray_status_t tray_status;
    char message[CONVEYOR_MQTT_PAYLOAD_MAX];

    if (status == NULL) {
        return;
    }

    if (!force && central_status_published && status->state == last_central_state) {
        return;
    }

    conveyor_job_get_tray_status(&tray_status);

    if (status->state == CONVEYOR_STATE_ERROR || status->state == CONVEYOR_STATE_ESTOP) {
        snprintf(message,
                 sizeof(message),
                 "{\"id\":\"%s\",\"state\":\"%s\",\"state_elapsed_ms\":%lu,\"s0\":%d,\"s1\":%d,\"has_tray\":%s,\"error\":\"%s\"}",
                 CONVEYOR_ID,
                 conveyor_state_name(status->state),
                 (unsigned long)status->state_elapsed_ms,
                 status->s0,
                 status->s1,
                 tray_status.has_tray ? "true" : "false",
                 status->error);
    } else {
        snprintf(message,
                 sizeof(message),
                 "{\"id\":\"%s\",\"state\":\"%s\",\"state_elapsed_ms\":%lu,\"s0\":%d,\"s1\":%d,\"has_tray\":%s}",
                 CONVEYOR_ID,
                 conveyor_state_name(status->state),
                 (unsigned long)status->state_elapsed_ms,
                 status->s0,
                 status->s1,
                 tray_status.has_tray ? "true" : "false");
    }

    publish_to_topic(CONVEYOR_MQTT_TOPIC_CENTRAL_STATUS, message);
    central_status_published = true;
    last_central_state = status->state;
}

static void publish_tray_status(bool force)
{
    conveyor_tray_status_t status;
    char message[128];

    if (!mqtt_connected || mqtt_client == NULL) {
        return;
    }

    conveyor_job_get_tray_status(&status);

    if (!force && tray_status_published && status.has_tray == last_has_tray) {
        return;
    }

    snprintf(message,
             sizeof(message),
             "{\"id\":\"%s\",\"has_tray\":%s,\"s0\":%d,\"s1\":%d}",
             CONVEYOR_ID,
             status.has_tray ? "true" : "false",
             status.s0,
             status.s1);

    esp_mqtt_client_publish(mqtt_client, CONVEYOR_MQTT_TOPIC_TRAY, message, 0, 0, 0);
    tray_status_published = true;
    last_has_tray = status.has_tray;
}

void mqtt_publish_job_status(const conveyor_status_t *status)
{
    char message[256];

    if (status == NULL) {
        return;
    }

    if (status->state == CONVEYOR_STATE_ERROR || status->state == CONVEYOR_STATE_ESTOP) {
        snprintf(message,
                 sizeof(message),
                 "{\"id\":\"%s\",\"state\":\"%s\",\"state_elapsed_ms\":%lu,\"error\":\"%s\",\"s0\":%d,\"s1\":%d}",
                 CONVEYOR_ID,
                 conveyor_state_name(status->state),
                 (unsigned long)status->state_elapsed_ms,
                 status->error,
                 status->s0,
                 status->s1);
    } else {
        snprintf(message,
                 sizeof(message),
                 "{\"id\":\"%s\",\"state\":\"%s\",\"state_elapsed_ms\":%lu,\"s0\":%d,\"s1\":%d}",
                 CONVEYOR_ID,
                 conveyor_state_name(status->state),
                 (unsigned long)status->state_elapsed_ms,
                 status->s0,
                 status->s1);
    }

    publish_text(message);
    publish_central_status(status, false);

    if (!central_command.active) {
        return;
    }

    if (status->state == CONVEYOR_STATE_ERROR) {
        publish_central_result(central_command.command_id, "failure", status->error);
        central_command.active = false;
        return;
    }

    if (central_command.type == CONVEYOR_CMD_START_TX && status->state == CONVEYOR_STATE_TX_DONE) {
        publish_central_result(central_command.command_id, "success", "transmit complete");
        central_command.active = false;
        return;
    }

    if (central_command.type == CONVEYOR_CMD_START_RX && status->state == CONVEYOR_STATE_RX_DONE) {
        publish_central_result(central_command.command_id, "success", "receive complete");
        central_command.active = false;
        return;
    }

    if (central_command.type == CONVEYOR_CMD_EMERGENCY_STOP && status->state == CONVEYOR_STATE_ESTOP) {
        publish_central_result(central_command.command_id, "success", "stop active");
        central_command.active = false;
        return;
    }

    if (central_command.type == CONVEYOR_CMD_CLEAR_ERROR && status->state == CONVEYOR_STATE_IDLE) {
        publish_central_result(central_command.command_id, "success", "error cleared");
        central_command.active = false;
    }
}

static void publish_bad_command(const char *error)
{
    conveyor_status_t status;
    char message[256];

    conveyor_job_get_status(&status);
    snprintf(message,
             sizeof(message),
             "{\"id\":\"%s\",\"state\":\"%s\",\"state_elapsed_ms\":%lu,\"error\":\"%s\",\"s0\":%d,\"s1\":%d}",
             CONVEYOR_ID,
             conveyor_state_name(status.state),
             (unsigned long)status.state_elapsed_ms,
             error,
             status.s0,
             status.s1);
    publish_text(message);
}

static bool reject_if_busy(void)
{
    if (!conveyor_job_is_idle()) {
        publish_bad_command("JOB_BUSY");
        return true;
    }

    return false;
}

static void handle_command_message(const char *message)
{
    conveyor_cmd_t command;

    if (strcmp(message, "{\"type\":\"tx\"}") == 0) {
        if (reject_if_busy()) {
            return;
        }

        if (!conveyor_job_has_tray()) {
            publish_bad_command("NO_TRAY");
            return;
        }

        command.type = CONVEYOR_CMD_START_TX;
        if (!conveyor_job_send_command(command)) {
            publish_bad_command("QUEUE_FULL");
        }
        return;
    }

    if (strcmp(message, "{\"type\":\"rx\"}") == 0) {
        if (reject_if_busy()) {
            return;
        }

        if (conveyor_job_has_tray()) {
            publish_bad_command("TRAY_PRESENT");
            return;
        }

        command.type = CONVEYOR_CMD_START_RX;
        if (!conveyor_job_send_command(command)) {
            publish_bad_command("QUEUE_FULL");
        }
        return;
    }

    if (strcmp(message, "{\"type\":\"emergency_stop\"}") == 0) {
        command.type = CONVEYOR_CMD_EMERGENCY_STOP;
        if (!conveyor_job_send_command(command)) {
            publish_bad_command("QUEUE_FULL");
        }
        return;
    }

    if (strcmp(message, "{\"type\":\"clear_error\"}") == 0) {
        command.type = CONVEYOR_CMD_CLEAR_ERROR;
        if (!conveyor_job_send_command(command)) {
            publish_bad_command("QUEUE_FULL");
        }
        return;
    }

    publish_bad_command("UNKNOWN_COMMAND");
}

static bool central_command_type_from_text(const char *type, conveyor_cmd_type_t *command_type)
{
    if (type == NULL || command_type == NULL) {
        return false;
    }

    if (strcmp(type, "transmit") == 0) {
        *command_type = CONVEYOR_CMD_START_TX;
        return true;
    }

    if (strcmp(type, "receive") == 0) {
        *command_type = CONVEYOR_CMD_START_RX;
        return true;
    }

    if (strcmp(type, "stop") == 0) {
        *command_type = CONVEYOR_CMD_EMERGENCY_STOP;
        return true;
    }

    if (strcmp(type, "clear_error") == 0) {
        *command_type = CONVEYOR_CMD_CLEAR_ERROR;
        return true;
    }

    return false;
}

static bool extract_json_string_field(const char *json, const char *field, char *value, size_t value_size)
{
    char key[32];
    const char *cursor = NULL;
    size_t length = 0;

    if (json == NULL || field == NULL || value == NULL || value_size == 0) {
        return false;
    }

    snprintf(key, sizeof(key), "\"%s\"", field);
    cursor = strstr(json, key);
    if (cursor == NULL) {
        return false;
    }

    cursor += strlen(key);
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n') {
        cursor++;
    }

    if (*cursor != ':') {
        return false;
    }

    cursor++;
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n') {
        cursor++;
    }

    if (*cursor != '"') {
        return false;
    }

    cursor++;
    while (cursor[length] != '\0' && cursor[length] != '"') {
        if (cursor[length] == '\\' || length + 1 >= value_size) {
            return false;
        }
        length++;
    }

    if (cursor[length] != '"') {
        return false;
    }

    memcpy(value, cursor, length);
    value[length] = '\0';
    return true;
}

static void track_central_command(const char *command_id, conveyor_cmd_type_t type)
{
    central_command.active = true;
    central_command.type = type;
    snprintf(central_command.command_id, sizeof(central_command.command_id), "%s", command_id);
}

static void handle_central_command_message(const char *message)
{
    char command_id[64];
    char type[32];
    conveyor_cmd_t command;
    conveyor_status_t status;

    command_id[0] = '\0';
    type[0] = '\0';

    (void)extract_json_string_field(message, "command_id", command_id, sizeof(command_id));
    if (!extract_json_string_field(message, "type", type, sizeof(type))) {
        (void)extract_json_string_field(message, "command", type, sizeof(type));
    }

    if (!is_safe_json_text(command_id)) {
        publish_central_result("", "failure", "missing or invalid command_id");
        return;
    }

    if (!central_command_type_from_text(type, &command.type)) {
        if (strcmp(type, "get_commands") == 0) {
            publish_central_command_list(command_id);
        } else {
            publish_central_result(command_id, "failure", "unknown command");
        }
        return;
    }

    if (central_command.active && command.type != CONVEYOR_CMD_EMERGENCY_STOP) {
        publish_central_result(command_id, "failure", "busy: command already active");
        return;
    }

    if (command.type == CONVEYOR_CMD_START_TX || command.type == CONVEYOR_CMD_START_RX) {
        if (!conveyor_job_is_idle()) {
            publish_central_result(command_id, "failure", "busy: job busy");
            return;
        }

        if (command.type == CONVEYOR_CMD_START_TX && !conveyor_job_has_tray()) {
            publish_central_result(command_id, "failure", "no tray");
            return;
        }

        if (command.type == CONVEYOR_CMD_START_RX && conveyor_job_has_tray()) {
            publish_central_result(command_id, "failure", "tray present");
            return;
        }
    }

    if (command.type == CONVEYOR_CMD_CLEAR_ERROR) {
        conveyor_job_get_status(&status);
        if (status.state != CONVEYOR_STATE_ERROR && status.state != CONVEYOR_STATE_ESTOP) {
            publish_central_result(command_id, "failure", "no error to clear");
            return;
        }
    }

    if (command.type == CONVEYOR_CMD_EMERGENCY_STOP && central_command.active) {
        publish_central_result(central_command.command_id, "failure", "interrupted by emergency stop");
        central_command.active = false;
    }

    if (!conveyor_job_send_command(command)) {
        publish_central_result(command_id, "failure", "queue full");
        return;
    }

    track_central_command(command_id, command.type);
    publish_central_result(command_id, "success", "received: command accepted");
}

static void handle_emergency_message(const char *message)
{
    conveyor_cmd_t command = {
        .type = CONVEYOR_CMD_EMERGENCY_STOP,
    };

    if (strcmp(message, "STOP") == 0 || strcmp(message, "{\"type\":\"emergency_stop\"}") == 0) {
        if (!conveyor_job_send_command(command)) {
            publish_bad_command("QUEUE_FULL");
        }
    } else {
        publish_bad_command("BAD_EMERGENCY");
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        mqtt_connected = false;
        ESP_LOGW(TAG, "WiFi disconnected, retrying");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "WiFi connected");
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    char topic[96];
    char message[CONVEYOR_MQTT_PAYLOAD_MAX];
    int topic_len = 0;
    int message_len = 0;

    (void)handler_args;
    (void)base;

    if (event_id == MQTT_EVENT_CONNECTED) {
        mqtt_connected = true;
        esp_mqtt_client_subscribe(mqtt_client, CONVEYOR_MQTT_TOPIC_CMD, 0);
        esp_mqtt_client_subscribe(mqtt_client, CONVEYOR_MQTT_TOPIC_EMERGENCY, 0);
        esp_mqtt_client_subscribe(mqtt_client, CONVEYOR_MQTT_TOPIC_ALL_EMERGENCY, 0);
        esp_mqtt_client_subscribe(mqtt_client, CONVEYOR_MQTT_TOPIC_CENTRAL_COMMAND, 0);
        publish_tray_status(true);
        ESP_LOGI(TAG, "MQTT connected");
        return;
    }

    if (event_id == MQTT_EVENT_DISCONNECTED) {
        mqtt_connected = false;
        ESP_LOGW(TAG, "MQTT disconnected");
        return;
    }

    if (event_id != MQTT_EVENT_DATA) {
        return;
    }

    topic_len = event->topic_len;
    if (topic_len >= (int)sizeof(topic)) {
        topic_len = sizeof(topic) - 1;
    }

    message_len = event->data_len;
    if (message_len >= (int)sizeof(message)) {
        message_len = sizeof(message) - 1;
    }

    memcpy(topic, event->topic, topic_len);
    topic[topic_len] = '\0';

    memcpy(message, event->data, message_len);
    message[message_len] = '\0';

    if (strcmp(topic, CONVEYOR_MQTT_TOPIC_CMD) == 0) {
        handle_command_message(message);
        return;
    }

    if (strcmp(topic, CONVEYOR_MQTT_TOPIC_CENTRAL_COMMAND) == 0) {
        handle_central_command_message(message);
        return;
    }

    if (strcmp(topic, CONVEYOR_MQTT_TOPIC_EMERGENCY) == 0 ||
        strcmp(topic, CONVEYOR_MQTT_TOPIC_ALL_EMERGENCY) == 0) {
        handle_emergency_message(message);
    }
}

static void wifi_init(void)
{
    wifi_config_t wifi_config = {0};

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));

    snprintf((char *)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), "%s", CONVEYOR_WIFI_SSID);
    snprintf((char *)wifi_config.sta.password, sizeof(wifi_config.sta.password), "%s", CONVEYOR_WIFI_PASS);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static void mqtt_init(void)
{
    esp_mqtt_client_config_t config = {
        .broker.address.uri = CONVEYOR_MQTT_BROKER_URI,
        .credentials.client_id = "conveyor_" CONVEYOR_ID,
    };

    mqtt_client = esp_mqtt_client_init(&config);
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(mqtt_client));
}

void mqtt_status_task(void *arg)
{
#if CONVEYOR_MQTT_STATUS_ENABLED
    conveyor_status_t status;
#endif

    (void)arg;

    while (1) {
#if CONVEYOR_MQTT_STATUS_ENABLED
        conveyor_job_get_status(&status);
        mqtt_publish_job_status(&status);
#endif
        publish_tray_status(false);
        vTaskDelay(pdMS_TO_TICKS(runtime_config_mqtt_status_period_ms()));
    }
}

void configure_mqtt(void)
{
    wifi_init();
    mqtt_init();
}
