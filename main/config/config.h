#ifndef CONFIG_H
#define CONFIG_H

#define CONVEYOR_ID "C0"                              /* Public conveyor name used in serial and MQTT messages. */

#define CONVEYOR_MQTT_ENABLED 1                       /* 1 starts WiFi/MQTT; 0 builds serial-only control. */
#define CONVEYOR_MQTT_STATUS_ENABLED 1                /* 1 enables periodic MQTT status publishing. */

#define CONVEYOR_WIFI_SSID "thrd_warehouse"           /* WiFi network name used by the ESP32 station. */
#define CONVEYOR_WIFI_PASS "thrd@789"                 /* WiFi password for CONVEYOR_WIFI_SSID. */
#define CONVEYOR_MQTT_BROKER_URI "mqtt://192.168.1.126" /* MQTT broker URI used after WiFi connects. */

#define CONVEYOR_MQTT_TOPIC_CMD "conveyor/C0/cmd"     /* Per-conveyor high-level job command topic. */
#define CONVEYOR_MQTT_TOPIC_EMERGENCY "conveyor/C0/emergency" /* Per-conveyor emergency-stop topic. */
#define CONVEYOR_MQTT_TOPIC_FEEDBACK "conveyor/C0/feedback" /* Per-conveyor job/status feedback topic. */
#define CONVEYOR_MQTT_TOPIC_ALL_EMERGENCY "conveyor/all/emergency" /* Shared emergency-stop topic for every conveyor. */
#define CONVEYOR_MQTT_TOPIC_TRAY "conveyor/C0/tray"   /* Tray-present status topic for this conveyor. */

#define CONVEYOR_RUN_PWM 64                           /* Default direct-PWM debug speed. Runtime key: run_pwm. */
#define CONVEYOR_RUN_SPEED_COUNTS_PER_SEC 5000         /* Default closed-loop job speed in encoder counts/sec. */
#define CONVEYOR_SPEED_KP_MILLI 10                    /* Default speed P gain scaled by 1000; 50 means 0.050. */
#define CONVEYOR_SPEED_KD_MILLI 10                    /* Default speed D gain scaled by 1000 and normalized to a 20 ms PID tick. */
#define CONVEYOR_PWM_SLEW_STEP 1                      /* Max PWM change per 20 ms motor PID tick. */
#define CONVEYOR_SPEED_PID_PWM_MAX 128                /* Highest PWM that speed control may command. */
#define CONVEYOR_MOTOR_FORWARD_DIRECTION 1            /* MD30C direction level used for normal conveyor travel. */

#define CONVEYOR_JOB_TASK_STACK_SIZE 4096             /* FreeRTOS stack size for the conveyor state-machine task. */
#define CONVEYOR_JOB_TASK_PRIORITY 5                  /* FreeRTOS priority for the conveyor state-machine task. */
#define CONVEYOR_JOB_QUEUE_LENGTH 8                   /* Number of pending high-level job commands allowed. */
#define CONVEYOR_JOB_TICK_MS 20                       /* State-machine update period in milliseconds. */
#define CONVEYOR_DONE_HOLD_MS 100                     /* Time DONE states remain visible before returning to IDLE. */

#define CONVEYOR_MQTT_STATUS_TASK_STACK_SIZE 4096     /* FreeRTOS stack size for periodic MQTT status task. */
#define CONVEYOR_MQTT_STATUS_TASK_PRIORITY 4          /* FreeRTOS priority for periodic MQTT status task. */
#define CONVEYOR_MQTT_STATUS_PERIOD_MS 100            /* Default period between MQTT status publishes. */
#define CONVEYOR_MQTT_PAYLOAD_MAX 160                 /* Max JSON payload buffer size for MQTT messages. */

#define CONVEYOR_TIMEOUT_TX_DETECT_MS 5000            /* TX max time waiting for tray to reach S1. */
#define CONVEYOR_TIMEOUT_TX_CLEAR_MS 5000             /* TX max time waiting for tray to clear S1. */
#define CONVEYOR_TIMEOUT_RX_DETECT_MS 5000            /* RX max time waiting for incoming tray at S0. */
#define CONVEYOR_TIMEOUT_RX_DONE_MS 5000              /* RX max time waiting for tray to reach S1 after S0. */

#endif
