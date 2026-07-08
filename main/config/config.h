#pragma once

// Compile-time machine config. Runtime-tunable config lives in runtime_config.c.

// Application identity. Change these values to retarget the generic axis firmware.
#define APP_AXIS_APP_NAME "cnc"
// Machine id used in console, MQTT topics, and persisted ownership.
#define APP_AXIS_MACHINE_ID "cnc2"
// MQTT client id can diverge from machine id later if needed.
#define APP_AXIS_MQTT_CLIENT_ID "cnc2"
// Topic segment for factory/<machine_id>/<topic_name> topics.
#define APP_AXIS_TOPIC_NAME "cnc"
// WiFi SSID for the later MQTT checkpoint.
#define APP_AXIS_WIFI_SSID "thrd_warehouse"
// WiFi password for the configured machine network.
#define APP_AXIS_WIFI_PASS "thrd@789"
// Broker URI for factory/<machine_id>/<topic_name> topics.
#define APP_AXIS_MQTT_URI "mqtt://192.168.1.183"

// Direction GPIO driven before PWM is applied.
#define APP_AXIS_DIR_PIN 17
// LEDC PWM output pin for motor speed.
#define APP_AXIS_PWM_PIN 18
// Positive-travel sensor GPIO.
#define APP_AXIS_POSITIVE_SENSOR_PIN 40
// Negative-travel sensor GPIO, commonly used as a reference point.
#define APP_AXIS_NEGATIVE_SENSOR_PIN 39
// Electrical level that means a physical sensor is active.
#define APP_AXIS_SENSOR_ACTIVE_LEVEL 1
// PCNT quadrature channel A input.
#define APP_AXIS_ENCODER_A_PIN 5
// PCNT quadrature channel B input.
#define APP_AXIS_ENCODER_B_PIN 4

// Direction GPIO level for positive travel.
#define APP_AXIS_POSITIVE_DIR_LEVEL 1
// Direction GPIO level for negative travel.
#define APP_AXIS_NEGATIVE_DIR_LEVEL 0
// LEDC PWM frequency for smooth motor drive.
#define APP_AXIS_PWM_FREQ_HZ 5000
// PID/control loop period in milliseconds.
#define APP_AXIS_CONTROL_PERIOD_MS 10
