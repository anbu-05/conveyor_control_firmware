#include "conveyor_job.h"

#include <stdio.h>
#include <string.h>

#include "app_state.h"
#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mqtt_task.h"
#include "runtime_config.h"

static QueueHandle_t conveyor_cmd_queue;
static conveyor_status_t current_status = {
    .state = CONVEYOR_STATE_IDLE,
    .state_elapsed_ms = 0,
    .error = "",
    .s0 = 1,
    .s1 = 1,
};
static uint32_t state_started_ms;

const char *conveyor_state_name(conveyor_state_t state)
{
    if (state == CONVEYOR_STATE_IDLE) {
        return "IDLE";
    }
    if (state == CONVEYOR_STATE_TX_WAIT_FOR_TX1_DETECT) {
        return "TX_WAIT_FOR_TX1_DETECT";
    }
    if (state == CONVEYOR_STATE_TX_WAIT_FOR_TX1_CLEAR) {
        return "TX_WAIT_FOR_TX1_CLEAR";
    }
    if (state == CONVEYOR_STATE_RX_WAIT_FOR_RX0) {
        return "RX_WAIT_FOR_RX0";
    }
    if (state == CONVEYOR_STATE_RX_WAIT_FOR_RX1) {
        return "RX_WAIT_FOR_RX1";
    }
    if (state == CONVEYOR_STATE_TX_DONE) {
        return "TX_DONE";
    }
    if (state == CONVEYOR_STATE_RX_DONE) {
        return "RX_DONE";
    }
    if (state == CONVEYOR_STATE_ERROR) {
        return "ERROR";
    }
    if (state == CONVEYOR_STATE_ESTOP) {
        return "ESTOP";
    }

    return "UNKNOWN";
}

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

void conveyor_job_get_status(conveyor_status_t *status)
{
    if (status == NULL) {
        return;
    }

    current_status.s0 = sensors[0].value;
    current_status.s1 = sensors[1].value;
    current_status.state_elapsed_ms = now_ms() - state_started_ms;
    current_status.direction = conveyor_get_travel_direction();
    *status = current_status;
}

bool conveyor_job_has_tray(void)
{
    return sensors[0].value == 0 || sensors[1].value == 0;
}

void conveyor_job_get_tray_status(conveyor_tray_status_t *status)
{
    if (status == NULL) {
        return;
    }

    status->s0 = sensors[0].value;
    status->s1 = sensors[1].value;
    status->has_tray = conveyor_job_has_tray();
}

bool conveyor_job_send_command(conveyor_cmd_t command)
{
    if (conveyor_cmd_queue == NULL) {
        return false;
    }

    return xQueueSend(conveyor_cmd_queue, &command, 0) == pdTRUE;
}

bool conveyor_job_is_idle(void)
{
    return current_status.state == CONVEYOR_STATE_IDLE;
}

static bool tray_detected(int sensor_index)
{
    return sensors[sensor_index].value == 0;
}

static int tx1_sensor(void)
{
    if (conveyor_get_travel_direction() == CONVEYOR_TRAVEL_S0_TO_S1) {
        return 1;
    }
    return 0;
}

static int rx0_sensor(void)
{
    if (conveyor_get_travel_direction() == CONVEYOR_TRAVEL_S0_TO_S1) {
        return 0;
    }
    return 1;
}

static int rx1_sensor(void)
{
    if (conveyor_get_travel_direction() == CONVEYOR_TRAVEL_S0_TO_S1) {
        return 1;
    }
    return 0;
}

static void publish_status(void)
{
    current_status.s0 = sensors[0].value;
    current_status.s1 = sensors[1].value;
    current_status.state_elapsed_ms = now_ms() - state_started_ms;
    current_status.direction = conveyor_get_travel_direction();

    console_printf("EVENT JOB %s %s\r\n",
                   runtime_config_conveyor_id(),
                   conveyor_state_name(current_status.state));
    mqtt_publish_job_status(&current_status);
}

static void set_state(conveyor_state_t state)
{
    current_status.state = state;
    state_started_ms = now_ms();
    publish_status();
}

static bool timeout_expired(uint32_t timeout_ms)
{
    return now_ms() - state_started_ms >= timeout_ms;
}

static void set_error(const char *error)
{
    stop_all_motors();
    snprintf(current_status.error, sizeof(current_status.error), "%s", error);
    set_state(CONVEYOR_STATE_ERROR);
}

static void start_tx(void)
{
    if (current_status.state != CONVEYOR_STATE_IDLE) {
        console_print("ERR JOB_BUSY\r\n");
        return;
    }

    if (!conveyor_job_has_tray()) {
        console_print("ERR NO_TRAY\r\n");
        return;
    }

    current_status.error[0] = '\0';
    (void)start_motor("M0");

    if (tray_detected(tx1_sensor())) {
        set_state(CONVEYOR_STATE_TX_WAIT_FOR_TX1_CLEAR);
    } else {
        set_state(CONVEYOR_STATE_TX_WAIT_FOR_TX1_DETECT);
    }
}

static void start_rx(void)
{
    if (current_status.state != CONVEYOR_STATE_IDLE) {
        console_print("ERR JOB_BUSY\r\n");
        return;
    }

    if (conveyor_job_has_tray()) {
        console_print("ERR TRAY_PRESENT\r\n");
        return;
    }

    current_status.error[0] = '\0';
    stop_all_motors();
    set_state(CONVEYOR_STATE_RX_WAIT_FOR_RX0);
}

static void emergency_stop(void)
{
    stop_all_motors();
    snprintf(current_status.error, sizeof(current_status.error), "ESTOP");
    set_state(CONVEYOR_STATE_ESTOP);
}

static void clear_error(void)
{
    if (current_status.state == CONVEYOR_STATE_ERROR || current_status.state == CONVEYOR_STATE_ESTOP) {
        current_status.error[0] = '\0';
        set_state(CONVEYOR_STATE_IDLE);
    }
}

static void handle_command(conveyor_cmd_t command)
{
    if (command.type == CONVEYOR_CMD_START_TX) {
        start_tx();
        return;
    }

    if (command.type == CONVEYOR_CMD_START_RX) {
        start_rx();
        return;
    }

    if (command.type == CONVEYOR_CMD_EMERGENCY_STOP) {
        emergency_stop();
        return;
    }

    if (command.type == CONVEYOR_CMD_CLEAR_ERROR) {
        clear_error();
        return;
    }
}

static void update_state(void)
{
    int sensor = 0;

    if (current_status.state == CONVEYOR_STATE_TX_WAIT_FOR_TX1_DETECT) {
        sensor = tx1_sensor();
        if (!conveyor_job_has_tray()) {
            set_error("TX_LOST_TRAY");
        } else if (tray_detected(sensor)) {
            set_state(CONVEYOR_STATE_TX_WAIT_FOR_TX1_CLEAR);
        } else if (timeout_expired(runtime_config_tx_detect_timeout_ms())) {
            set_error("TX_DETECT_TIMEOUT");
        }
        return;
    }

    if (current_status.state == CONVEYOR_STATE_TX_WAIT_FOR_TX1_CLEAR) {
        sensor = tx1_sensor();
        if (!tray_detected(sensor)) {
            (void)stop_motor("M0");
            set_state(CONVEYOR_STATE_TX_DONE);
        } else if (timeout_expired(runtime_config_tx_clear_timeout_ms())) {
            set_error("TX_CLEAR_TIMEOUT");
        }
        return;
    }

    if (current_status.state == CONVEYOR_STATE_RX_WAIT_FOR_RX0) {
        sensor = rx0_sensor();
        if (tray_detected(sensor)) {
            (void)start_motor("M0");
            set_state(CONVEYOR_STATE_RX_WAIT_FOR_RX1);
        } else if (timeout_expired(runtime_config_rx_detect_timeout_ms())) {
            set_error("RX_DETECT_TIMEOUT");
        }
        return;
    }

    if (current_status.state == CONVEYOR_STATE_RX_WAIT_FOR_RX1) {
        sensor = rx1_sensor();
        if (!conveyor_job_has_tray()) {
            set_error("RX_LOST_TRAY");
        } else if (tray_detected(sensor)) {
            (void)stop_motor("M0");
            set_state(CONVEYOR_STATE_RX_DONE);
        } else if (timeout_expired(runtime_config_rx_done_timeout_ms())) {
            set_error("RX_DONE_TIMEOUT");
        }
        return;
    }

    if (current_status.state == CONVEYOR_STATE_TX_DONE || current_status.state == CONVEYOR_STATE_RX_DONE) {
        if (timeout_expired(runtime_config_done_hold_ms())) {
            set_state(CONVEYOR_STATE_IDLE);
        }
    }
}

void conveyor_job_task(void *arg)
{
    conveyor_cmd_t command;

    (void)arg;

    if (conveyor_cmd_queue == NULL) {
        console_print("ERR JOB_QUEUE\r\n");
        vTaskDelete(NULL);
        return;
    }

    set_state(CONVEYOR_STATE_IDLE);

    while (1) {
        while (xQueueReceive(conveyor_cmd_queue, &command, 0) == pdTRUE) {
            handle_command(command);
        }

        update_state();
        vTaskDelay(pdMS_TO_TICKS(CONVEYOR_JOB_TICK_MS));
    }
}

void configure_conveyor_job(void)
{
    conveyor_cmd_queue = xQueueCreate(CONVEYOR_JOB_QUEUE_LENGTH, sizeof(conveyor_cmd_t));
    if (conveyor_cmd_queue == NULL) {
        console_print("ERR JOB_QUEUE\r\n");
    }
}
