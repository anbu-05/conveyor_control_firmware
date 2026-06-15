#include "app_state.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_err.h"

/*
 * Configures the shared LEDC timer and then attaches each BTS7960 RPWM/LPWM
 * pin to its channel. Enable pins are held low until a nonzero PWM is applied.
 */
void configure_pwm(void)
{
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = MOTOR_PWM_RESOLUTION,
        .freq_hz = MOTOR_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };

    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    for (int i = 0; i < MOTOR_COUNT; i++) {
        ledc_channel_config_t rpwm_channel = {
            .gpio_num = motors[i].rpwm_gpio,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = motors[i].rpwm_channel,
            .timer_sel = LEDC_TIMER_0,
            .duty = 0,
            .hpoint = 0,
        };
        ledc_channel_config_t lpwm_channel = {
            .gpio_num = motors[i].lpwm_gpio,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = motors[i].lpwm_channel,
            .timer_sel = LEDC_TIMER_0,
            .duty = 0,
            .hpoint = 0,
        };
        gpio_config_t enable_gpio_config = {
            .pin_bit_mask = (1ULL << motors[i].ren_gpio) | (1ULL << motors[i].len_gpio),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };

        ESP_ERROR_CHECK(gpio_config(&enable_gpio_config));
        ESP_ERROR_CHECK(gpio_set_level(motors[i].ren_gpio, 0));
        ESP_ERROR_CHECK(gpio_set_level(motors[i].len_gpio, 0));
        ESP_ERROR_CHECK(ledc_channel_config(&rpwm_channel));
        ESP_ERROR_CHECK(ledc_channel_config(&lpwm_channel));
    }
}
