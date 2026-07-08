/*
 * Learned axis-state persistence implementation placeholder.
 * Persistence restore must never cause motion at boot; restored state is only
 * informational until a later explicit command requests movement.
 */

#include "store/axis_store.h"

/* Initializes future learned-state persistence without causing boot-time motion. */
esp_err_t axis_store_init(void)
{
    return ESP_OK;
}
