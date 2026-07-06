#include "mqtt_task.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_state.h"
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

static void publish_bad_command(const char *error);

bool mqtt_task_is_connected(void)
{
    return mqtt_connected;
}

void mqtt_task_refresh_subscription(const char *old_topic, const char *new_topic)
{
    if (!mqtt_connected || mqtt_client == NULL || old_topic == NULL || new_topic == NULL) {
        return;
    }

    if (strcmp(old_topic, new_topic) == 0) {
        return;
    }

    esp_mqtt_client_unsubscribe(mqtt_client, old_topic);
    esp_mqtt_client_subscribe(mqtt_client, new_topic, 0);
}

static void publish_text(const char *text)
{
    if (!mqtt_connected || mqtt_client == NULL || text == NULL) {
        return;
    }

    esp_mqtt_client_publish(mqtt_client, runtime_config_mqtt_topic_feedback(), text, 0, 0, 0);
}

static void publish_direction(void)
{
    char message[96];
    conveyor_travel_direction_t direction = conveyor_get_travel_direction();

    snprintf(message,
             sizeof(message),
             "{\"id\":\"%s\",\"direction\":\"%s\"}",
             CONVEYOR_ID,
             conveyor_travel_direction_name(direction));
    publish_text(message);
}

static void publish_rssi(void)
{
    char message[64];
    int rssi = conveyor_get_rssi();

    if (rssi == INT16_MIN) {
        publish_bad_command("RSSI_UNAVAILABLE");
        return;
    }

    snprintf(message, sizeof(message), "{\"id\":\"%s\",\"rssi\":%d}", CONVEYOR_ID, rssi);
    publish_text(message);
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

    esp_mqtt_client_publish(mqtt_client, runtime_config_mqtt_topic_tray(), message, 0, 0, 0);
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
                 "{\"id\":\"%s\",\"state\":\"%s\",\"state_elapsed_ms\":%lu,\"error\":\"%s\",\"s0\":%d,\"s1\":%d,\"direction\":\"%s\"}",
                 CONVEYOR_ID,
                 conveyor_state_name(status->state),
                 (unsigned long)status->state_elapsed_ms,
                 status->error,
                 status->s0,
                 status->s1,
                 conveyor_travel_direction_name(status->direction));
    } else {
        snprintf(message,
                 sizeof(message),
                 "{\"id\":\"%s\",\"state\":\"%s\",\"state_elapsed_ms\":%lu,\"s0\":%d,\"s1\":%d,\"direction\":\"%s\"}",
                 CONVEYOR_ID,
                 conveyor_state_name(status->state),
                 (unsigned long)status->state_elapsed_ms,
                 status->s0,
                 status->s1,
                 conveyor_travel_direction_name(status->direction));
    }

    publish_text(message);
}

static void publish_bad_command(const char *error)
{
    conveyor_status_t status;
    char message[256];

    conveyor_job_get_status(&status);
    snprintf(message,
             sizeof(message),
             "{\"id\":\"%s\",\"state\":\"%s\",\"state_elapsed_ms\":%lu,\"error\":\"%s\",\"s0\":%d,\"s1\":%d,\"direction\":\"%s\"}",
             CONVEYOR_ID,
             conveyor_state_name(status.state),
             (unsigned long)status.state_elapsed_ms,
             error,
             status.s0,
             status.s1,
             conveyor_travel_direction_name(status.direction));
    publish_text(message);
}

static bool message_starts_with(const char *message, const char *prefix)
{
    return strncmp(message, prefix, strlen(prefix)) == 0;
}

static bool reject_if_busy(void)
{
    if (!conveyor_job_is_idle()) {
        publish_bad_command("JOB_BUSY");
        return true;
    }

    return false;
}

static bool parse_gain_milli(const char *text, const char *config_name, int32_t *value)
{
    int32_t whole = 0;
    int32_t fraction = 0;
    int fraction_digits = 0;
    int digit_count = 0;
    int i = 0;

    if (text == NULL || value == NULL || text[0] == '\0') {
        return false;
    }

    while (text[i] >= '0' && text[i] <= '9') {
        whole = whole * 10 + (text[i] - '0');
        digit_count++;
        i++;
    }

    if (text[i] == '.') {
        i++;
        while (text[i] >= '0' && text[i] <= '9' && fraction_digits < 3) {
            fraction = fraction * 10 + (text[i] - '0');
            fraction_digits++;
            digit_count++;
            i++;
        }
    }

    if (text[i] != '\0' || digit_count == 0) {
        return false;
    }

    while (fraction_digits < 3) {
        fraction = fraction * 10;
        fraction_digits++;
    }

    *value = whole * 1000 + fraction;
    return runtime_config_value_is_valid(config_name, *value);
}

static bool parse_gain_payload(const char *message, const char *prefix, const char *config_name, int32_t *value)
{
    char text[16];
    int prefix_len = strlen(prefix);
    int value_len = 0;
    int i = 0;

    if (strncmp(message, prefix, prefix_len) != 0) {
        return false;
    }

    while (message[prefix_len + value_len] != '\0' &&
           strcmp(&message[prefix_len + value_len], "\"}") != 0) {
        value_len++;
    }

    if (value_len <= 0 || value_len >= (int)sizeof(text)) {
        return false;
    }

    if (strcmp(&message[prefix_len + value_len], "\"}") != 0) {
        return false;
    }

    for (i = 0; i < value_len; i++) {
        text[i] = message[prefix_len + i];
    }
    text[value_len] = '\0';

    return parse_gain_milli(text, config_name, value);
}

static void publish_gain_config(const char *name, int32_t value)
{
    char message[128];

    snprintf(message,
             sizeof(message),
             "{\"id\":\"%s\",\"config\":\"%s\",\"value\":\"%ld.%03ld\"}",
             CONVEYOR_ID,
             name,
             (long)(value / 1000),
             (long)(value % 1000));
    publish_text(message);
}

static void publish_all_gain_config(void)
{
    char message[160];
    int32_t kp_milli = runtime_config_speed_kp_milli();
    int32_t kd_milli = runtime_config_speed_kd_milli();

    snprintf(message,
             sizeof(message),
             "{\"id\":\"%s\",\"config\":\"speed_gains\",\"speed_kp\":\"%ld.%03ld\",\"speed_kd\":\"%ld.%03ld\"}",
             CONVEYOR_ID,
             (long)(kp_milli / 1000),
             (long)(kp_milli % 1000),
             (long)(kd_milli / 1000),
             (long)(kd_milli % 1000));
    publish_text(message);
}

static void handle_command_message(const char *message)
{
    conveyor_cmd_t command;
    int32_t gain_milli = 0;

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

    if (strncmp(message, "{\"type\":\"setkp\",\"value\":\"", strlen("{\"type\":\"setkp\",\"value\":\"")) == 0) {
        if (!parse_gain_payload(message, "{\"type\":\"setkp\",\"value\":\"", "speed_kp_milli", &gain_milli)) {
            publish_bad_command("BAD_VALUE");
            return;
        }

        if (!runtime_config_set_speed_kp_milli(gain_milli)) {
            publish_bad_command("CONFIG_SAVE");
            return;
        }

        publish_gain_config("speed_kp", gain_milli);
        return;
    }

    if (strncmp(message, "{\"type\":\"setkd\",\"value\":\"", strlen("{\"type\":\"setkd\",\"value\":\"")) == 0) {
        if (!parse_gain_payload(message, "{\"type\":\"setkd\",\"value\":\"", "speed_kd_milli", &gain_milli)) {
            publish_bad_command("BAD_VALUE");
            return;
        }

        if (!runtime_config_set_speed_kd_milli(gain_milli)) {
            publish_bad_command("CONFIG_SAVE");
            return;
        }

        publish_gain_config("speed_kd", gain_milli);
        return;
    }

    if (strcmp(message, "{\"type\":\"resetk\"}") == 0) {
        if (!runtime_config_reset_speed_gains()) {
            publish_bad_command("CONFIG_SAVE");
            return;
        }

        publish_all_gain_config();
        return;
    }

    if (strcmp(message, "{\"type\":\"setdirection\",\"value\":\"s0tos1\"}") == 0) {
        if (reject_if_busy()) {
            return;
        }
        conveyor_set_travel_direction(CONVEYOR_TRAVEL_S0_TO_S1);
        publish_direction();
        return;
    }

    if (strcmp(message, "{\"type\":\"setdirection\",\"value\":\"s1tos0\"}") == 0) {
        if (reject_if_busy()) {
            return;
        }
        conveyor_set_travel_direction(CONVEYOR_TRAVEL_S1_TO_S0);
        publish_direction();
        return;
    }

    if (message_starts_with(message, "{\"type\":\"setdirection\",\"value\":")) {
        publish_bad_command("BAD_VALUE");
        return;
    }

    if (strcmp(message, "{\"type\":\"getdirection\"}") == 0) {
        publish_direction();
        return;
    }

    if (strcmp(message, "{\"type\":\"getrssi\"}") == 0) {
        publish_rssi();
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
        esp_mqtt_client_subscribe(mqtt_client, runtime_config_mqtt_topic_cmd(), 0);
        esp_mqtt_client_subscribe(mqtt_client, runtime_config_mqtt_topic_emergency(), 0);
        esp_mqtt_client_subscribe(mqtt_client, runtime_config_mqtt_topic_all_emergency(), 0);
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

    if (strcmp(topic, runtime_config_mqtt_topic_cmd()) == 0) {
        handle_command_message(message);
        return;
    }

    if (strcmp(topic, runtime_config_mqtt_topic_emergency()) == 0 ||
        strcmp(topic, runtime_config_mqtt_topic_all_emergency()) == 0) {
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

    snprintf((char *)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), "%s", runtime_config_wifi_ssid());
    snprintf((char *)wifi_config.sta.password, sizeof(wifi_config.sta.password), "%s", runtime_config_wifi_pass());

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static void mqtt_init(void)
{
    esp_mqtt_client_config_t config = {
        .broker.address.uri = runtime_config_mqtt_broker_uri(),
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
