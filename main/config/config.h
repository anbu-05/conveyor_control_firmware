#pragma once

// Compile-time machine config. Runtime-tunable config lives in runtime_config.c.

#include "driver/gpio.h"

// Application identity. Change these values to retarget the generic motor firmware.
#define APP_MOTOR_APP_NAME "conveyor"
// Machine id used in console, MQTT topics, and persisted ownership.
#define APP_MOTOR_MACHINE_ID "C1"
// MQTT client id can diverge from machine id later if needed.
#define APP_MOTOR_MQTT_CLIENT_ID "factory"
// Topic segment for factory/<machine_id>/<topic_name> topics.
#define APP_MOTOR_TOPIC_NAME "command"
// WiFi SSID for the later MQTT checkpoint.
#define APP_MOTOR_WIFI_SSID "thrd_warehouse"
// WiFi password for the configured machine network.
#define APP_MOTOR_WIFI_PASS "thrd@789"
// Broker URI for factory/<machine_id>/<topic_name> topics.
#define APP_MOTOR_MQTT_URI "mqtt://192.168.1.183"

// BTS7960 right/forward PWM input.
#define MOTOR_RPWM_GPIO GPIO_NUM_16
// BTS7960 left/reverse PWM input.
#define MOTOR_LPWM_GPIO GPIO_NUM_15
// BTS7960 right enable pin.
#define MOTOR_REN_GPIO GPIO_NUM_7
// BTS7960 left enable pin.
#define MOTOR_LEN_GPIO GPIO_NUM_8
// Upstream conveyor sensor GPIO.
#define APP_MOTOR_UPSTREAM_SENSOR_PIN 4
// Downstream conveyor sensor GPIO.
#define APP_MOTOR_DOWNSTREAM_SENSOR_PIN 5
// Electrical level that means a physical sensor is active.
#define APP_MOTOR_SENSOR_ACTIVE_LEVEL 0
// PCNT quadrature channel A input.
#define APP_MOTOR_ENCODER_A_PIN 17
// PCNT quadrature channel B input.
#define APP_MOTOR_ENCODER_B_PIN 18

// Console/API direction value for positive travel.
#define APP_MOTOR_POSITIVE_DIR_LEVEL 1
// Console/API direction value for negative travel.
#define APP_MOTOR_NEGATIVE_DIR_LEVEL 0
// LEDC PWM frequency for smooth motor drive.
#define APP_MOTOR_PWM_FREQ_HZ 5000
// PID/control loop period in milliseconds.
#define APP_MOTOR_CONTROL_PERIOD_MS 10
