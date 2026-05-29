#ifndef RUNTIME_CONFIG_H
#define RUNTIME_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

void configure_runtime_config(void); /* Loads defaults, initializes NVS, then applies saved values. */

int runtime_config_run_pwm(void); /* Direct-PWM debug value from run_pwm. */
int runtime_config_run_speed_counts_per_sec(void); /* Job speed target from run_speed_counts_per_sec. */
int runtime_config_speed_kp_milli(void); /* Speed P gain scaled by 1000. */
int runtime_config_speed_kd_milli(void); /* Speed D gain scaled by 1000. */
uint32_t runtime_config_done_hold_ms(void); /* DONE-state hold time before IDLE. */
uint32_t runtime_config_tx_detect_timeout_ms(void); /* TX wait-for-S1-detect timeout. */
uint32_t runtime_config_tx_clear_timeout_ms(void); /* TX wait-for-S1-clear timeout. */
uint32_t runtime_config_rx_detect_timeout_ms(void); /* RX wait-for-S0-detect timeout. */
uint32_t runtime_config_rx_done_timeout_ms(void); /* RX wait-for-S1-detect timeout. */
uint32_t runtime_config_mqtt_status_period_ms(void); /* MQTT status publish period. */

bool runtime_config_get_value(const char *name, int32_t *value); /* Reads one runtime config value by serial key name. */
bool runtime_config_value_is_valid(const char *name, int32_t value); /* Checks one runtime config value before saving. */
bool runtime_config_set_value(const char *name, int32_t value); /* Saves one runtime config value to NVS and RAM. */
bool runtime_config_set_speed_kp_milli(int32_t value); /* Saves speed_kp_milli; used by the setkp command. */
bool runtime_config_set_speed_kd_milli(int32_t value); /* Saves speed_kd_milli; used by the setkd command. */
bool runtime_config_reset_speed_gains(void); /* Saves default speed_kp_milli and speed_kd_milli. */
bool runtime_config_reset_defaults(void); /* Saves compile-time defaults back to NVS and RAM. */
void runtime_config_print_all(void); /* Prints every runtime config value as CONFIG lines. */

#endif
