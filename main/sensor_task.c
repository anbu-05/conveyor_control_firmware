#include "app_state.h"

#include "driver/gpio.h"
#include "esp_err.h"

/*
 * Configures all binary sensor pins as plain inputs.
 * External pullups are used, so the ESP32 internal pullup and pulldown are off.
 */
void configure_sensors(void)
{
    for (int i = 0; i < SENSOR_COUNT; i++) {
        gpio_config_t sensor_gpio_config = {
            .pin_bit_mask = 1ULL << sensors[i].gpio,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };

        ESP_ERROR_CHECK(gpio_config(&sensor_gpio_config));
        sensors[i].value = gpio_get_level(sensors[i].gpio);
        sensors[i].last_value = sensors[i].value;
    }
}

/*
 * FreeRTOS task for binary sensor polling.
 * It records every reading and prints an event line only when watching is on
 * and a sensor value changes.
 */
void sensor_reader_task(void *arg)
{
    int value = 0;

    (void)arg;

    while (1) {
        for (int i = 0; i < SENSOR_COUNT; i++) {
            value = gpio_get_level(sensors[i].gpio);

            if (value != sensors[i].last_value) {
                sensors[i].value = value;

                if (sensor_watch_enabled) {
                    console_printf("EVENT SENSOR %s %d %d\r\n", sensors[i].name, sensors[i].last_value, value);
                }

                sensors[i].last_value = value;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(SENSOR_POLL_DELAY_MS));
    }
}
