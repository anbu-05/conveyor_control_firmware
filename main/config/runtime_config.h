#ifndef RUNTIME_CONFIG_H
#define RUNTIME_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

void configure_runtime_config(void);

int runtime_config_run_pwm(void);
uint32_t runtime_config_done_hold_ms(void);
uint32_t runtime_config_tx_detect_timeout_ms(void);
uint32_t runtime_config_tx_clear_timeout_ms(void);
uint32_t runtime_config_rx_detect_timeout_ms(void);
uint32_t runtime_config_rx_done_timeout_ms(void);
uint32_t runtime_config_mqtt_status_period_ms(void);

bool runtime_config_get_value(const char *name, int32_t *value);
bool runtime_config_value_is_valid(const char *name, int32_t value);
bool runtime_config_set_value(const char *name, int32_t value);
bool runtime_config_reset_defaults(void);
void runtime_config_print_all(void);

#endif
