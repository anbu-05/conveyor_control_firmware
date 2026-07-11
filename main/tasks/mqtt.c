/*
 * Backend-facing MQTT command/result/status task.
 * This module owns networking for the current checkpoint and translates JSON
 * commands into the same public state-machine jobs used by console control.
 */

#include "tasks/mqtt.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "config/config.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "shared/app_state.h"
#include "statemachine/statemachine.h"

#define MQTT_COMMAND_ID_MAX_LEN 96
#define MQTT_COMMAND_QUEUE_LENGTH 8
#define MQTT_TOPIC_MAX_LEN 96
#define MQTT_STATUS_TASK_STACK_SIZE 4096
#define MQTT_STATUS_TASK_PRIORITY 3
#define MQTT_STATUS_POLL_MS 250

typedef enum {
    MQTT_COMMAND_ACK_TEST,
    MQTT_COMMAND_TRAY_RECEIVE,
    MQTT_COMMAND_TRAY_TRANSMIT,
    MQTT_COMMAND_GET_COMMANDS,
} mqtt_command_id_t;

typedef struct {
    const char *name;
    mqtt_command_id_t id;
} mqtt_command_entry_t;

typedef struct {
    mqtt_command_id_t command;
    char command_id[MQTT_COMMAND_ID_MAX_LEN];
} mqtt_command_t;

static const char *TAG = APP_MOTOR_APP_NAME;

/* MQTT follows console's table-plus-switch shape so adding commands stays local and auditable. */
static const mqtt_command_entry_t s_commands[] = {
    {"ack_test", MQTT_COMMAND_ACK_TEST},
    {"tray_receive", MQTT_COMMAND_TRAY_RECEIVE},
    {"tray_transmit", MQTT_COMMAND_TRAY_TRANSMIT},
    {"get_commands", MQTT_COMMAND_GET_COMMANDS},
};

static esp_mqtt_client_handle_t s_client;
static QueueHandle_t s_command_queue;
static TaskHandle_t s_status_task_handle;
static bool s_connected;
static bool s_mqtt_started;
static char s_command_topic[MQTT_TOPIC_MAX_LEN];
static char s_result_topic[MQTT_TOPIC_MAX_LEN];
static char s_node_status_topic[MQTT_TOPIC_MAX_LEN];
static const char *s_last_backend_status;
static bool s_last_has_tray;
static bool s_have_last_node_status;

/* State-machine result tokens stay private to avoid expanding the state-machine public API for MQTT formatting. */
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

/* Backend status intentionally compresses internal states to the documented node_status vocabulary. */
static const char *backend_status_text(statemachine_status_t status)
{
    switch (status) {
    case STATEMACHINE_STATUS_IDLE:
        return "idle";
    case STATEMACHINE_STATUS_RECEIVE_WAITING_FOR_TRAY:
    case STATEMACHINE_STATUS_RECEIVE_MOVING_TRAY:
    case STATEMACHINE_STATUS_RECEIVE_TRAY_RECEIVED:
        return "receiving";
    case STATEMACHINE_STATUS_TRANSMIT_TRANSMITTING_TRAY:
    case STATEMACHINE_STATUS_TRANSMIT_TRAY_HANDED_OFF:
        return "transmitting";
    }

    return "unknown";
}

/* Result publishing centralizes the common result envelope and preserves the original backend command_id. */
static void publish_result_payload(cJSON *payload)
{
    char *text = NULL;

    if (payload == NULL || s_client == NULL || !s_connected) {
        return;
    }

    text = cJSON_PrintUnformatted(payload);
    if (text == NULL) {
        ESP_LOGE(TAG, "failed to render MQTT result JSON");
        return;
    }

    (void)esp_mqtt_client_publish(s_client, s_result_topic, text, 0, 1, 0);
    cJSON_free(text);
}

static void publish_result(const char *command_id, const char *command_status, const char *message)
{
    cJSON *payload = cJSON_CreateObject();

    if (payload == NULL) {
        ESP_LOGE(TAG, "failed to allocate MQTT result JSON");
        return;
    }

    cJSON_AddStringToObject(payload, "command_id", command_id);
    cJSON_AddStringToObject(payload, "command_status", command_status);
    cJSON_AddStringToObject(payload, "message", message);
    publish_result_payload(payload);
    cJSON_Delete(payload);
}

/* get_commands reports only commands implemented in this firmware checkpoint, with no movement params yet. */
static void publish_commands_result(const char *command_id)
{
    cJSON *payload = cJSON_CreateObject();
    cJSON *commands = NULL;

    if (payload == NULL) {
        ESP_LOGE(TAG, "failed to allocate get_commands JSON");
        return;
    }

    cJSON_AddStringToObject(payload, "command_id", command_id);
    cJSON_AddStringToObject(payload, "command_status", "success");
    cJSON_AddStringToObject(payload, "message", "supported commands");
    commands = cJSON_AddArrayToObject(payload, "commands");

    for (size_t i = 0; commands != NULL && i < sizeof(s_commands) / sizeof(s_commands[0]); i++) {
        cJSON *entry = cJSON_CreateObject();
        cJSON *required_params = cJSON_CreateArray();
        cJSON *optional_params = cJSON_CreateArray();

        if (entry == NULL || required_params == NULL || optional_params == NULL) {
            ESP_LOGE(TAG, "failed to allocate command entry JSON");
            cJSON_Delete(entry);
            cJSON_Delete(required_params);
            cJSON_Delete(optional_params);
            break;
        }

        cJSON_AddStringToObject(entry, "command", s_commands[i].name);
        cJSON_AddItemToObject(entry, "required_params", required_params);
        cJSON_AddItemToObject(entry, "optional_params", optional_params);
        cJSON_AddItemToArray(commands, entry);
    }

    publish_result_payload(payload);
    cJSON_Delete(payload);
}

/* Tray presence is derived from the protected sensor snapshot, not from internal state-machine status. */
static bool get_has_tray(void)
{
    int upstream_sensor = !APP_MOTOR_SENSOR_ACTIVE_LEVEL;
    int downstream_sensor = !APP_MOTOR_SENSOR_ACTIVE_LEVEL;

    if (motor_mutex != NULL) {
        xSemaphoreTake(motor_mutex, portMAX_DELAY);
    }
    upstream_sensor = motors[0].upstream_sensor;
    downstream_sensor = motors[0].downstream_sensor;
    if (motor_mutex != NULL) {
        xSemaphoreGive(motor_mutex);
    }

    return upstream_sensor == APP_MOTOR_SENSOR_ACTIVE_LEVEL ||
           downstream_sensor == APP_MOTOR_SENSOR_ACTIVE_LEVEL;
}

/* Node status publishes only backend-facing state and tray presence when either value changes. */
static void publish_node_status_if_changed(void)
{
    const char *backend_status = backend_status_text(statemachine_get_status());
    const bool has_tray = get_has_tray();
    cJSON *payload = NULL;
    char *text = NULL;

    if (!s_connected || (s_have_last_node_status && s_last_backend_status == backend_status && s_last_has_tray == has_tray)) {
        return;
    }

    payload = cJSON_CreateObject();
    if (payload == NULL) {
        ESP_LOGE(TAG, "failed to allocate node_status JSON");
        return;
    }

    cJSON_AddStringToObject(payload, "id", APP_MOTOR_MACHINE_ID);
    cJSON_AddStringToObject(payload, "status", backend_status);
    cJSON_AddBoolToObject(payload, "has_tray", has_tray);
    text = cJSON_PrintUnformatted(payload);
    if (text != NULL) {
        (void)esp_mqtt_client_publish(s_client, s_node_status_topic, text, 0, 1, 0);
        cJSON_free(text);
        s_last_backend_status = backend_status;
        s_last_has_tray = has_tray;
        s_have_last_node_status = true;
    } else {
        ESP_LOGE(TAG, "failed to render node_status JSON");
    }

    cJSON_Delete(payload);
}

/* A private status task keeps node_status moving while mqtt_task blocks on tray jobs. */
static void mqtt_status_task(void *arg)
{
    (void)arg;

    while (true) {
        publish_node_status_if_changed();
        vTaskDelay(pdMS_TO_TICKS(MQTT_STATUS_POLL_MS));
    }
}

/* MQTT command lookup uses the same command table that get_commands reports. */
static bool find_command(const char *name, mqtt_command_id_t *out_command)
{
    for (size_t i = 0; i < sizeof(s_commands) / sizeof(s_commands[0]); i++) {
        if (strcmp(name, s_commands[i].name) == 0) {
            *out_command = s_commands[i].id;
            return true;
        }
    }

    return false;
}

/* MQTT callbacks only parse, reject, or enqueue so network callbacks never run blocking tray jobs. */
static void handle_mqtt_data(const esp_mqtt_event_handle_t event)
{
    cJSON *payload = cJSON_ParseWithLength(event->data, event->data_len);
    const cJSON *command_id_json = NULL;
    const cJSON *command_json = NULL;
    mqtt_command_t command = {0};

    if (payload == NULL) {
        /* Malformed JSON has no trustworthy command envelope, so log it instead of hand-parsing a partial command_id. */
        ESP_LOGW(TAG, "dropping malformed MQTT command");
        return;
    }

    command_id_json = cJSON_GetObjectItemCaseSensitive(payload, "command_id");
    if (!cJSON_IsString(command_id_json) || command_id_json->valuestring == NULL ||
        command_id_json->valuestring[0] == '\0' || strlen(command_id_json->valuestring) >= sizeof(command.command_id)) {
        ESP_LOGW(TAG, "dropping MQTT command without usable command_id");
        cJSON_Delete(payload);
        return;
    }
    strlcpy(command.command_id, command_id_json->valuestring, sizeof(command.command_id));

    command_json = cJSON_GetObjectItemCaseSensitive(payload, "command");
    if (!cJSON_IsString(command_json) || command_json->valuestring == NULL) {
        publish_result(command.command_id, "failure", "missing command");
        cJSON_Delete(payload);
        return;
    }
    if (!find_command(command_json->valuestring, &command.command)) {
        publish_result(command.command_id, "failure", "unknown command");
        cJSON_Delete(payload);
        return;
    }

    if (xQueueSend(s_command_queue, &command, 0) != pdTRUE) {
        publish_result(command.command_id, "failure", "rejected: mqtt command queue full");
        cJSON_Delete(payload);
        return;
    }

    publish_result(command.command_id, "received", "command received");
    cJSON_Delete(payload);
}

/* WiFi events reconnect the station and start MQTT only after TCP/IP is available. */
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == WIFI_EVENT && (event_id == WIFI_EVENT_STA_START || event_id == WIFI_EVENT_STA_DISCONNECTED)) {
        if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            s_connected = false;
        }
        (void)esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP && s_client != NULL && !s_mqtt_started) {
        const esp_err_t err = esp_mqtt_client_start(s_client);

        if (err == ESP_OK) {
            s_mqtt_started = true;
        } else {
            ESP_LOGE(TAG, "failed to start MQTT client: %s", esp_err_to_name(err));
        }
    }
}

/* MQTT events keep connection state local and subscribe only to this configured conveyor's command topic. */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    const esp_mqtt_event_handle_t event = event_data;

    (void)handler_args;
    (void)base;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        (void)esp_mqtt_client_subscribe(s_client, s_command_topic, 1);
        publish_node_status_if_changed();
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        break;
    case MQTT_EVENT_DATA:
        if (event != NULL && event->topic != NULL && event->data != NULL &&
            event->topic_len == (int)strlen(s_command_topic) &&
            strncmp(event->topic, s_command_topic, event->topic_len) == 0) {
            handle_mqtt_data(event);
        }
        break;
    default:
        break;
    }
}

/* Topics are built from APP_MOTOR_MACHINE_ID so one firmware image can be retargeted by config. */
static esp_err_t build_topics(void)
{
    int written = snprintf(s_command_topic, sizeof(s_command_topic),
                           "factory/conveyor/%s/command", APP_MOTOR_MACHINE_ID);

    if (written < 0 || written >= (int)sizeof(s_command_topic)) {
        return ESP_ERR_INVALID_SIZE;
    }
    written = snprintf(s_result_topic, sizeof(s_result_topic),
                       "factory/conveyor/%s/result", APP_MOTOR_MACHINE_ID);
    if (written < 0 || written >= (int)sizeof(s_result_topic)) {
        return ESP_ERR_INVALID_SIZE;
    }
    written = snprintf(s_node_status_topic, sizeof(s_node_status_topic),
                       "factory/conveyor/%s/node_status", APP_MOTOR_MACHINE_ID);
    if (written < 0 || written >= (int)sizeof(s_node_status_topic)) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

/* MQTT owns WiFi setup for this checkpoint and uses the normal ESP-IDF STA lifecycle. */
static esp_err_t init_wifi(void)
{
    wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config_t wifi_config = {0};
    esp_err_t err = esp_netif_init();

    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    (void)esp_netif_create_default_wifi_sta();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi_init_config), TAG, "initialize WiFi");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                            &wifi_event_handler, NULL, NULL),
                        TAG, "register WiFi event handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                            &wifi_event_handler, NULL, NULL),
                        TAG, "register IP event handler");

    strlcpy((char *)wifi_config.sta.ssid, APP_MOTOR_WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, APP_MOTOR_WIFI_PASS, sizeof(wifi_config.sta.password));

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set WiFi STA mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG, "set WiFi config");
    return esp_wifi_start();
}

/* Initializes command routing, topics, WiFi, MQTT, and the private live status publisher. */
esp_err_t mqtt_init(void)
{
    const esp_mqtt_client_config_t mqtt_config = {
        .broker.address.uri = APP_MOTOR_MQTT_URI,
        .credentials.client_id = APP_MOTOR_MQTT_CLIENT_ID,
    };

    /* Make the command mailbox where MQTT callbacks can drop work for mqtt_task(). */
    if (s_command_queue == NULL) {
        s_command_queue = xQueueCreate(MQTT_COMMAND_QUEUE_LENGTH, sizeof(mqtt_command_t));
        if (s_command_queue == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    /* Build this conveyor's command/result/status topic names from APP_MOTOR_MACHINE_ID. */
    ESP_RETURN_ON_ERROR(build_topics(), TAG, "build MQTT topics");

    /* Create the MQTT client object that will connect to APP_MOTOR_MQTT_URI. */
    s_client = esp_mqtt_client_init(&mqtt_config);
    if (s_client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* Tell ESP-IDF which callback handles MQTT connect, disconnect, and data events. */
    ESP_RETURN_ON_ERROR(esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                                       mqtt_event_handler, NULL),
                        TAG, "register MQTT event handler");

    /* Start WiFi; MQTT starts later when the WiFi event handler sees an IP address. */
    ESP_RETURN_ON_ERROR(init_wifi(), TAG, "initialize WiFi");

    /* Start a small private task that publishes node_status while tray jobs may block mqtt_task(). */
    if (s_status_task_handle == NULL) {
        const BaseType_t created = xTaskCreate(mqtt_status_task, "mqtt_status_task",
                                               MQTT_STATUS_TASK_STACK_SIZE, NULL,
                                               MQTT_STATUS_TASK_PRIORITY, &s_status_task_handle);
        if (created != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
    }

    return ESP_OK;
}

/* Runs the command handler loop; blocking tray jobs are isolated here, outside MQTT callbacks. */
void mqtt_task(void *arg)
{
    mqtt_command_t command = {0};

    (void)arg;

    while (true) {
        if (s_command_queue == NULL) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (xQueueReceive(s_command_queue, &command, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        switch (command.command) {
        case MQTT_COMMAND_ACK_TEST:
            publish_result(command.command_id, "success", "ack test ok");
            break;

        case MQTT_COMMAND_TRAY_RECEIVE:
        {
            statemachine_result_t result = STATEMACHINE_RESULT_JOB_REJECTED;

            result = statemachine_jobrx();
            if (result == STATEMACHINE_RESULT_RX_DONE) {
                publish_result(command.command_id, "success", "tray receive complete");
            } else {
                publish_result(command.command_id, "failure", statemachine_result_text(result));
            }
            break;
        }

        case MQTT_COMMAND_TRAY_TRANSMIT:
        {
            statemachine_result_t result = STATEMACHINE_RESULT_JOB_REJECTED;

            result = statemachine_jobtx();
            if (result == STATEMACHINE_RESULT_TX_DONE) {
                publish_result(command.command_id, "success", "tray transmit complete");
            } else {
                publish_result(command.command_id, "failure", statemachine_result_text(result));
            }
            break;
        }

        case MQTT_COMMAND_GET_COMMANDS:
            publish_commands_result(command.command_id);
            break;
        }
    }
}
