#include "sd_event_logger.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>

#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdmmc_cmd.h"

#define SDLOG_MOUNT_POINT "/sdcard"
#define SDLOG_LINE_MAX 384
#define SDLOG_PATH_MAX 320
#define SDLOG_DEFAULT_QUEUE_LENGTH 64
#define SDLOG_DEFAULT_STACK_SIZE 4096
#define SDLOG_DEFAULT_PRIORITY 3
#define SDLOG_DEFAULT_FLUSH_MS 1000

typedef enum {
    SDLOG_MSG_SYSTEM,
    SDLOG_MSG_FLUSH,
} sdlog_msg_type_t;

typedef struct {
    sdlog_msg_type_t type;
    char line[SDLOG_LINE_MAX];
} sdlog_msg_t;

static const char *TAG = "sdlog";

static QueueHandle_t s_queue;
static SemaphoreHandle_t s_file_mutex;
static FILE *s_system_file;
static FILE *s_timeline_file;
static FILE *s_actuators_file;
static FILE *s_inputs_file;
static char s_session_path[SDLOG_PATH_MAX];
static char s_time_topic[48];
static uint32_t s_last_epoch;
static uint32_t s_last_epoch_at_ms;
static uint32_t s_command_id;
static bool s_ready;
static int s_flush_period_ms = SDLOG_DEFAULT_FLUSH_MS;
static vprintf_like_t s_old_vprintf;
static volatile bool s_in_log_hook;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000LL);
}

static uint32_t current_epoch(void)
{
    uint32_t ms = now_ms();

    if (s_last_epoch == 0) {
        return 0;
    }

    return s_last_epoch + ((ms - s_last_epoch_at_ms) / 1000);
}

static void mkdir_if_missing(const char *path)
{
    struct stat st;

    if (stat(path, &st) != 0) {
        (void)mkdir(path, 0775);
    }
}

static void csv_text(FILE *file, const char *text)
{
    if (text == NULL) {
        return;
    }

    fputc('"', file);
    for (int i = 0; text[i] != '\0'; i++) {
        if (text[i] == '"') {
            fputc('"', file);
        }
        fputc(text[i], file);
    }
    fputc('"', file);
}

static bool get_field(const char *line, const char *key, char *out, size_t out_size)
{
    char pattern[32];
    const char *start = NULL;
    const char *end = NULL;
    size_t len = 0;

    if (line == NULL || key == NULL || out == NULL || out_size == 0) {
        return false;
    }

    snprintf(pattern, sizeof(pattern), "%s=", key);
    start = strstr(line, pattern);
    if (start == NULL) {
        out[0] = '\0';
        return false;
    }

    start += strlen(pattern);
    end = strchr(start, ' ');
    if (end == NULL) {
        end = start + strlen(start);
    }

    len = (size_t)(end - start);
    if (len >= out_size) {
        len = out_size - 1;
    }

    memcpy(out, start, len);
    out[len] = '\0';
    return true;
}

static const char *message_after_tag(const char *line, const char *tag)
{
    const char *p = strstr(line, tag);

    if (p == NULL) {
        return NULL;
    }

    p = strchr(p, ':');
    if (p == NULL) {
        return NULL;
    }

    p++;
    while (*p == ' ') {
        p++;
    }

    return p;
}

static void trim_newline(char *text)
{
    int len = 0;

    if (text == NULL) {
        return;
    }

    len = strlen(text);
    while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r')) {
        text[len - 1] = '\0';
        len--;
    }
}

static bool join_path(char *out, size_t out_size, const char *left, const char *right)
{
    size_t left_len = 0;
    size_t right_len = 0;

    if (out == NULL || out_size == 0 || left == NULL || right == NULL) {
        return false;
    }

    left_len = strlen(left);
    right_len = strlen(right);
    if (left_len + 1 + right_len + 1 > out_size) {
        return false;
    }

    memcpy(out, left, left_len);
    out[left_len] = '/';
    memcpy(out + left_len + 1, right, right_len);
    out[left_len + 1 + right_len] = '\0';
    return true;
}

static void write_timeline(const char *event_type, const char *message)
{
    char command_id[24] = "";
    char source[32] = "";
    char topic[96] = "";
    char from_state[48] = "";
    char to_state[48] = "";
    char result[32] = "";
    char error[48] = "";
    char text[160] = "";

    if (s_timeline_file == NULL || event_type == NULL || message == NULL) {
        return;
    }

    (void)get_field(message, "command_id", command_id, sizeof(command_id));
    (void)get_field(message, "source", source, sizeof(source));
    (void)get_field(message, "topic", topic, sizeof(topic));
    (void)get_field(message, "from", from_state, sizeof(from_state));
    (void)get_field(message, "to", to_state, sizeof(to_state));
    (void)get_field(message, "result", result, sizeof(result));
    (void)get_field(message, "error", error, sizeof(error));
    if (!get_field(message, "text", text, sizeof(text))) {
        (void)get_field(message, "payload", text, sizeof(text));
    }
    if (error[0] == '\0') {
        (void)get_field(message, "code", error, sizeof(error));
    }
    if (to_state[0] == '\0') {
        (void)get_field(message, "state", to_state, sizeof(to_state));
    }
    trim_newline(text);

    fprintf(s_timeline_file, "%lu,%lu,",
            (unsigned long)now_ms(),
            (unsigned long)current_epoch());
    csv_text(s_timeline_file, event_type);
    fputc(',', s_timeline_file);
    csv_text(s_timeline_file, command_id);
    fputc(',', s_timeline_file);
    csv_text(s_timeline_file, source);
    fputc(',', s_timeline_file);
    csv_text(s_timeline_file, topic);
    fputc(',', s_timeline_file);
    csv_text(s_timeline_file, from_state);
    fputc(',', s_timeline_file);
    csv_text(s_timeline_file, to_state);
    fputc(',', s_timeline_file);
    csv_text(s_timeline_file, result);
    fputc(',', s_timeline_file);
    csv_text(s_timeline_file, error);
    fputc(',', s_timeline_file);
    csv_text(s_timeline_file, text);
    fputc('\n', s_timeline_file);
}

static void write_actuator(const char *message)
{
    char id[32] = "";
    char type[24] = "";
    char target_pos[24] = "";
    char current_pos[24] = "";
    char target_speed[24] = "";
    char current_speed[24] = "";
    char pwm[16] = "";
    char direction[16] = "";
    char current[24] = "";
    char statusword[24] = "";
    char status[32] = "";
    char error_code[32] = "";
    char mode[24] = "";

    if (s_actuators_file == NULL || message == NULL) {
        return;
    }

    (void)get_field(message, "id", id, sizeof(id));
    (void)get_field(message, "type", type, sizeof(type));
    (void)get_field(message, "target_pos", target_pos, sizeof(target_pos));
    (void)get_field(message, "current_pos", current_pos, sizeof(current_pos));
    (void)get_field(message, "target_speed", target_speed, sizeof(target_speed));
    (void)get_field(message, "current_speed", current_speed, sizeof(current_speed));
    (void)get_field(message, "pwm", pwm, sizeof(pwm));
    (void)get_field(message, "direction", direction, sizeof(direction));
    (void)get_field(message, "current", current, sizeof(current));
    (void)get_field(message, "statusword", statusword, sizeof(statusword));
    (void)get_field(message, "status", status, sizeof(status));
    (void)get_field(message, "error_code", error_code, sizeof(error_code));
    (void)get_field(message, "mode", mode, sizeof(mode));
    if (current_pos[0] == '\0') {
        (void)get_field(message, "position", current_pos, sizeof(current_pos));
    }

    fprintf(s_actuators_file,
            "%lu,%lu,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n",
            (unsigned long)now_ms(),
            (unsigned long)current_epoch(),
            id,
            type,
            target_pos,
            current_pos,
            target_speed,
            current_speed,
            pwm,
            direction,
            current,
            statusword,
            status,
            error_code,
            mode);
}

static void write_input(const char *message)
{
    char id[32] = "";
    char value[16] = "";
    char last_value[16] = "";
    char active[16] = "";

    if (s_inputs_file == NULL || message == NULL) {
        return;
    }

    (void)get_field(message, "id", id, sizeof(id));
    (void)get_field(message, "value", value, sizeof(value));
    (void)get_field(message, "last_value", last_value, sizeof(last_value));
    (void)get_field(message, "active", active, sizeof(active));

    fprintf(s_inputs_file,
            "%lu,%lu,%s,%s,%s,%s\n",
            (unsigned long)now_ms(),
            (unsigned long)current_epoch(),
            id,
            value,
            last_value,
            active);
}

static void route_special_log(const char *line)
{
    const char *message = NULL;

    message = message_after_tag(line, "SDLOG_CMD");
    if (message != NULL) {
        write_timeline("command", message);
        return;
    }

    message = message_after_tag(line, "SDLOG_STATE");
    if (message != NULL) {
        write_timeline("state", message);
        return;
    }

    message = message_after_tag(line, "SDLOG_MQTT");
    if (message != NULL) {
        write_timeline("mqtt", message);
        return;
    }

    message = message_after_tag(line, "SDLOG_TIME");
    if (message != NULL) {
        write_timeline("time", message);
        return;
    }

    message = message_after_tag(line, "SDLOG_SAFETY");
    if (message != NULL) {
        write_timeline("safety", message);
        return;
    }

    message = message_after_tag(line, "SDLOG_ERROR");
    if (message != NULL) {
        write_timeline("error", message);
        return;
    }

    message = message_after_tag(line, "SDLOG_EVENT");
    if (message != NULL) {
        write_timeline("event", message);
        return;
    }

    message = message_after_tag(line, "SDLOG_ACT");
    if (message != NULL) {
        write_actuator(message);
        return;
    }

    message = message_after_tag(line, "SDLOG_STATUS");
    if (message != NULL) {
        write_actuator(message);
        return;
    }

    message = message_after_tag(line, "SDLOG_INPUT");
    if (message != NULL) {
        write_input(message);
    }
}

static void flush_files(void)
{
    if (s_system_file != NULL) {
        fflush(s_system_file);
    }
    if (s_timeline_file != NULL) {
        fflush(s_timeline_file);
    }
    if (s_actuators_file != NULL) {
        fflush(s_actuators_file);
    }
    if (s_inputs_file != NULL) {
        fflush(s_inputs_file);
    }
}

static void sdlog_task(void *arg)
{
    sdlog_msg_t msg;
    TickType_t flush_ticks = pdMS_TO_TICKS(s_flush_period_ms);

    (void)arg;
    if (flush_ticks < 1) {
        flush_ticks = 1;
    }

    while (1) {
        if (xQueueReceive(s_queue, &msg, flush_ticks) == pdTRUE) {
            if (msg.type == SDLOG_MSG_SYSTEM && s_system_file != NULL) {
                xSemaphoreTake(s_file_mutex, portMAX_DELAY);
                fputs(msg.line, s_system_file);
                route_special_log(msg.line);
                xSemaphoreGive(s_file_mutex);
            } else if (msg.type == SDLOG_MSG_FLUSH) {
                xSemaphoreTake(s_file_mutex, portMAX_DELAY);
                flush_files();
                xSemaphoreGive(s_file_mutex);
            }
        } else {
            xSemaphoreTake(s_file_mutex, portMAX_DELAY);
            flush_files();
            xSemaphoreGive(s_file_mutex);
        }
    }
}

static int sdlog_vprintf(const char *format, va_list args)
{
    va_list copy;
    sdlog_msg_t msg;
    int written = 0;

    va_copy(copy, args);
    if (s_old_vprintf != NULL) {
        written = s_old_vprintf(format, args);
    } else {
        written = vprintf(format, args);
    }

    if (s_ready && s_queue != NULL && !s_in_log_hook) {
        s_in_log_hook = true;
        memset(&msg, 0, sizeof(msg));
        msg.type = SDLOG_MSG_SYSTEM;
        (void)vsnprintf(msg.line, sizeof(msg.line), format, copy);
        (void)xQueueSend(s_queue, &msg, 0);
        s_in_log_hook = false;
    }

    va_end(copy);
    return written;
}

static uint32_t next_session_number(void)
{
    nvs_handle_t handle;
    uint32_t session = 1;

    if (nvs_open("sdlog", NVS_READWRITE, &handle) != ESP_OK) {
        return session;
    }

    (void)nvs_get_u32(handle, "session", &session);
    (void)nvs_set_u32(handle, "session", session + 1);
    (void)nvs_commit(handle);
    nvs_close(handle);
    return session;
}

static FILE *open_session_file(const char *name, const char *mode)
{
    char path[SDLOG_PATH_MAX];

    if (!join_path(path, sizeof(path), s_session_path, name)) {
        return NULL;
    }
    return fopen(path, mode);
}

static esp_err_t mount_sd_card(const sdlog_config_t *config)
{
    sdmmc_card_t *card = NULL;
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };
    spi_bus_config_t bus_config = {
        .mosi_io_num = config->sd_mosi_gpio,
        .miso_io_num = config->sd_miso_gpio,
        .sclk_io_num = config->sd_sclk_gpio,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    esp_err_t err = spi_bus_initialize(host.slot, &bus_config, SDSPI_DEFAULT_DMA);

    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    slot_config.gpio_cs = config->sd_cs_gpio;
    slot_config.host_id = host.slot;

    return esp_vfs_fat_sdspi_mount(SDLOG_MOUNT_POINT, &host, &slot_config, &mount_config, &card);
}

static void write_session_files(const sdlog_config_t *config, uint32_t session_number)
{
    FILE *file = open_session_file("session.txt", "w");

    if (file != NULL) {
        fprintf(file, "log_format=1\n");
        fprintf(file, "device_type=%s\n", config->device_type);
        fprintf(file, "device_id=%s\n", config->device_id);
        fprintf(file, "project=%s\n", config->project);
        fprintf(file, "firmware_version=%s\n", config->firmware_version);
        fprintf(file, "session_id=session_%06lu\n", (unsigned long)session_number);
        fprintf(file, "boot_ms=0\n");
        fprintf(file, "time_topic=%s\n", s_time_topic);
        fprintf(file, "last_epoch=0\n");
        fprintf(file, "last_epoch_at_ms=0\n");
        fclose(file);
    }

    file = open_session_file("config.txt", "w");
    if (file != NULL) {
        if (config->config_text != NULL) {
            fputs(config->config_text, file);
        }
        fclose(file);
    }
}

esp_err_t sdlog_start(const sdlog_config_t *config)
{
    char path[SDLOG_PATH_MAX];
    uint32_t session_number = 0;
    esp_err_t err;

    if (config == NULL || config->device_type == NULL || config->device_id == NULL || config->project == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_ready) {
        return ESP_OK;
    }

    s_file_mutex = xSemaphoreCreateMutex();
    if (s_file_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_queue = xQueueCreate(config->queue_length > 0 ? config->queue_length : SDLOG_DEFAULT_QUEUE_LENGTH,
                           sizeof(sdlog_msg_t));
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_flush_period_ms = config->flush_period_ms > 0 ? config->flush_period_ms : SDLOG_DEFAULT_FLUSH_MS;
    snprintf(s_time_topic, sizeof(s_time_topic), "%s", config->time_topic != NULL ? config->time_topic : "all/time");

    err = mount_sd_card(config);
    if (err != ESP_OK) {
        return err;
    }

    session_number = next_session_number();
    snprintf(path, sizeof(path), "%s/%s", SDLOG_MOUNT_POINT, config->device_type);
    mkdir_if_missing(path);
    snprintf(path, sizeof(path), "%s/%s/%s", SDLOG_MOUNT_POINT, config->device_type, config->device_id);
    mkdir_if_missing(path);
    snprintf(s_session_path,
             sizeof(s_session_path),
             "%s/%s/%s/session_%06lu",
             SDLOG_MOUNT_POINT,
             config->device_type,
             config->device_id,
             (unsigned long)session_number);
    mkdir_if_missing(s_session_path);
    if (join_path(path, sizeof(path), s_session_path, "custom")) {
        mkdir_if_missing(path);
    }

    write_session_files(config, session_number);

    s_system_file = open_session_file("system.log", "a");
    s_timeline_file = open_session_file("timeline.csv", "a");
    s_actuators_file = open_session_file("actuators.csv", "a");
    s_inputs_file = open_session_file("inputs.csv", "a");
    if (s_system_file == NULL || s_timeline_file == NULL || s_actuators_file == NULL || s_inputs_file == NULL) {
        return ESP_FAIL;
    }

    fprintf(s_timeline_file, "ms,epoch,event_type,command_id,source,topic,from_state,to_state,result,error,text\n");
    fprintf(s_actuators_file, "ms,epoch,actuator_id,actuator_type,target_pos,current_pos,target_speed,current_speed,pwm,direction,current,statusword,status,error_code,mode\n");
    fprintf(s_inputs_file, "ms,epoch,input_id,value,last_value,active\n");
    flush_files();

    s_ready = true;
    s_old_vprintf = esp_log_set_vprintf(sdlog_vprintf);

    xTaskCreate(sdlog_task,
                "sdlog",
                config->stack_size > 0 ? config->stack_size : SDLOG_DEFAULT_STACK_SIZE,
                NULL,
                config->task_priority > 0 ? config->task_priority : SDLOG_DEFAULT_PRIORITY,
                NULL);

    ESP_LOGI(TAG, "started session %s", s_session_path);
    return ESP_OK;
}

bool sdlog_is_ready(void)
{
    return s_ready;
}

uint32_t sdlog_next_command_id(void)
{
    s_command_id++;
    if (s_command_id == 0) {
        s_command_id = 1;
    }
    return s_command_id;
}

void sdlog_update_epoch(uint32_t epoch)
{
    FILE *file = NULL;

    if (epoch == 0) {
        return;
    }

    s_last_epoch = epoch;
    s_last_epoch_at_ms = now_ms();

    if (s_ready) {
        file = open_session_file("session.txt", "a");
        if (file != NULL) {
            fprintf(file, "last_epoch=%lu\n", (unsigned long)s_last_epoch);
            fprintf(file, "last_epoch_at_ms=%lu\n", (unsigned long)s_last_epoch_at_ms);
            fclose(file);
        }
        ESP_LOGI("SDLOG_TIME", "source=mqtt topic=%s result=accepted text=%lu",
                 s_time_topic,
                 (unsigned long)epoch);
    }
}

void sdlog_custom_header(const char *name, const char *header)
{
    char path[SDLOG_PATH_MAX];
    char custom_path[SDLOG_PATH_MAX];
    char filename[64];
    FILE *file = NULL;

    if (!s_ready || name == NULL || header == NULL || strchr(name, '/') != NULL || strlen(name) > 48) {
        return;
    }

    snprintf(filename, sizeof(filename), "%s.csv", name);
    if (!join_path(custom_path, sizeof(custom_path), s_session_path, "custom") ||
        !join_path(path, sizeof(path), custom_path, filename)) {
        return;
    }
    if (access(path, F_OK) == 0) {
        return;
    }

    xSemaphoreTake(s_file_mutex, portMAX_DELAY);
    file = fopen(path, "w");
    if (file != NULL) {
        fprintf(file, "%s\n", header);
        fclose(file);
    }
    xSemaphoreGive(s_file_mutex);
}

void sdlog_custom_row(const char *name, const char *format, ...)
{
    char path[SDLOG_PATH_MAX];
    char custom_path[SDLOG_PATH_MAX];
    char filename[64];
    FILE *file = NULL;
    va_list args;

    if (!s_ready || name == NULL || format == NULL || strchr(name, '/') != NULL || strlen(name) > 48) {
        return;
    }

    snprintf(filename, sizeof(filename), "%s.csv", name);
    if (!join_path(custom_path, sizeof(custom_path), s_session_path, "custom") ||
        !join_path(path, sizeof(path), custom_path, filename)) {
        return;
    }

    xSemaphoreTake(s_file_mutex, portMAX_DELAY);
    file = fopen(path, "a");
    if (file != NULL) {
        va_start(args, format);
        vfprintf(file, format, args);
        va_end(args);
        fputc('\n', file);
        fclose(file);
    }
    xSemaphoreGive(s_file_mutex);
}
