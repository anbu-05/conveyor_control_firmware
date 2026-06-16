#ifndef SD_EVENT_LOGGER_H
#define SD_EVENT_LOGGER_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

typedef struct {
    const char *device_type;
    const char *device_id;
    const char *project;
    const char *firmware_version;
    const char *time_topic;
    const char *config_text;
    gpio_num_t sd_cs_gpio;
    gpio_num_t sd_mosi_gpio;
    gpio_num_t sd_sclk_gpio;
    gpio_num_t sd_miso_gpio;
    int queue_length;
    int stack_size;
    int task_priority;
    int flush_period_ms;
} sdlog_config_t;

esp_err_t sdlog_start(const sdlog_config_t *config);
bool sdlog_is_ready(void);
uint32_t sdlog_next_command_id(void);
void sdlog_update_epoch(uint32_t epoch);
void sdlog_custom_header(const char *name, const char *header);
void sdlog_custom_row(const char *name, const char *format, ...);

#endif
