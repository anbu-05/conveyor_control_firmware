#include "app_state.h"

#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#include "esp_err.h"

void configure_encoders(void)
{
    for (int i = 0; i < MOTOR_COUNT; i++) {
        pcnt_unit_config_t unit_config = {
            .low_limit = ENCODER_PCNT_LOW_LIMIT,
            .high_limit = ENCODER_PCNT_HIGH_LIMIT,
            .flags.accum_count = true,
        };
        pcnt_channel_handle_t channel_a = NULL;
        pcnt_channel_handle_t channel_b = NULL;
        pcnt_glitch_filter_config_t filter_config = {
            .max_glitch_ns = ENCODER_GLITCH_FILTER_NS,
        };
        pcnt_chan_config_t channel_a_config = {
            .edge_gpio_num = motors[i].encoder_a_gpio,
            .level_gpio_num = motors[i].encoder_b_gpio,
        };
        pcnt_chan_config_t channel_b_config = {
            .edge_gpio_num = motors[i].encoder_b_gpio,
            .level_gpio_num = motors[i].encoder_a_gpio,
        };
        gpio_config_t encoder_gpio_config = {
            .pin_bit_mask = (1ULL << motors[i].encoder_a_gpio) | (1ULL << motors[i].encoder_b_gpio),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };

        ESP_ERROR_CHECK(gpio_config(&encoder_gpio_config));
        ESP_ERROR_CHECK(pcnt_new_unit(&unit_config, &motors[i].pcnt_unit));
        ESP_ERROR_CHECK(pcnt_unit_set_glitch_filter(motors[i].pcnt_unit, &filter_config));
        ESP_ERROR_CHECK(pcnt_new_channel(motors[i].pcnt_unit, &channel_a_config, &channel_a));
        ESP_ERROR_CHECK(pcnt_new_channel(motors[i].pcnt_unit, &channel_b_config, &channel_b));

        ESP_ERROR_CHECK(pcnt_channel_set_edge_action(channel_a,
                                                     PCNT_CHANNEL_EDGE_ACTION_DECREASE,
                                                     PCNT_CHANNEL_EDGE_ACTION_INCREASE));
        ESP_ERROR_CHECK(pcnt_channel_set_level_action(channel_a,
                                                      PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                                      PCNT_CHANNEL_LEVEL_ACTION_INVERSE));
        ESP_ERROR_CHECK(pcnt_channel_set_edge_action(channel_b,
                                                     PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                                     PCNT_CHANNEL_EDGE_ACTION_DECREASE));
        ESP_ERROR_CHECK(pcnt_channel_set_level_action(channel_b,
                                                      PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                                      PCNT_CHANNEL_LEVEL_ACTION_INVERSE));

        ESP_ERROR_CHECK(pcnt_unit_add_watch_point(motors[i].pcnt_unit, ENCODER_PCNT_HIGH_LIMIT));
        ESP_ERROR_CHECK(pcnt_unit_add_watch_point(motors[i].pcnt_unit, ENCODER_PCNT_LOW_LIMIT));
        ESP_ERROR_CHECK(pcnt_unit_enable(motors[i].pcnt_unit));
        ESP_ERROR_CHECK(pcnt_unit_clear_count(motors[i].pcnt_unit));
        ESP_ERROR_CHECK(pcnt_unit_start(motors[i].pcnt_unit));
    }
}
