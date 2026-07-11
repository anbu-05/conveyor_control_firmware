/*
 * Minimal NVS initialization.
 * WiFi uses ESP-IDF's NVS-backed storage path today; fuller persistence for
 * settings and calibration remains future work in this module.
 */

#include "tasks/nvs.h"

#include "nvs_flash.h"

esp_err_t nvs_init(void)
{
    esp_err_t err = nvs_flash_init();

    /* Recover the standard NVS partition states that require erasing before WiFi can use storage. */
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase();
        if (err != ESP_OK) {
            return err;
        }
        err = nvs_flash_init();
    }

    return err;
}
