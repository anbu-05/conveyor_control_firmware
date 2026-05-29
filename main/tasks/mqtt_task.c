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

bool mqtt_task_is_connected(void)
{
    return mqtt_connected;
}

static void publish_text(const char *text)
{
    if (!mqtt_connected || mqtt_client == NULL || text == NULL) {
        return;
    }

    esp_mqtt_client_publish(mqtt_client, CONVEYOR_MQTT_TOPIC_FEEDBACK, text, 0, 0, 0);
}

void mqtt_publish_job_status(const conveyor_status_t *status)
{
    char message[192];

    if (status == NULL) {
        return;
    }

    if (status->state == CONVEYOR_STATE_ERROR || status->state == CONVEYOR_STATE_ESTOP) {
        snprintf(message,
                 sizeof(message),
                 "{\"id\":\"%s\",\"state\":\"%s\",\"error\":\"%s\",\"s0\":%d,\"s1\":%d}",
                 CONVEYOR_ID,
                 conveyor_state_name(status->state),
                 status->error,
                 status->s0,
                 status->s1);
    } else {
        snprintf(message,
                 sizeof(message),
                 "{\"id\":\"%s\",\"state\":\"%s\",\"s0\":%d,\"s1\":%d}",
                 CONVEYOR_ID,
                 conveyor_state_name(status->state),
                 status->s0,
                 status->s1);
    }

    publish_text(message);
}

static void publish_bad_command(const char *error)
{
    char message[96];

    snprintf(message, sizeof(message), "{\"id\":\"%s\",\"state\":\"ERROR\",\"error\":\"%s\"}", CONVEYOR_ID, error);
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
    conveyor_status_t status;

    (void)arg;

    while (1) {
        conveyor_job_get_status(&status);
        mqtt_publish_job_status(&status);
        vTaskDelay(pdMS_TO_TICKS(runtime_config_mqtt_status_period_ms()));
    }
}

void configure_mqtt(void)
{
    wifi_init();
    mqtt_init();
}
