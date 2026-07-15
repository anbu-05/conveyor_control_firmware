/*
 * Conveyor tray state-machine implementation.
 * This module accepts receive/transmit jobs, owns their timing, and drives the
 * conveyor toward upstream until sensors prove the job is done or failed.
 */

#include "statemachine/statemachine.h"

#include <stdbool.h>

#include "config/config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "tasks/hardware.h"
#include "tasks/pid.h"

#define STATEMACHINE_QUEUE_LEN 1
#define STATEMACHINE_POLL_MS 20
#define STATEMACHINE_MOVE_PWM 180
#define STATEMACHINE_RECEIVE_WAIT_TIMEOUT_MS 5000
#define STATEMACHINE_TRAVEL_TIMEOUT_MS 5000
#define STATEMACHINE_HANDOFF_TIMEOUT_MS 5000
#define STATEMACHINE_JOB_TIMEOUT_MS 12000

static const char *TAG = APP_MOTOR_APP_NAME;

typedef enum {
    STATEMACHINE_JOB_RECEIVE,
    STATEMACHINE_JOB_TRANSMIT,
} statemachine_job_t;

typedef enum {
    RECEIVE_WAITING_FOR_TRAY,
    RECEIVE_MOVING_TRAY,
    RECEIVE_TRAY_RECEIVED,
} receive_state_t;

typedef enum {
    TRANSMIT_TRANSMITTING_TRAY,
    TRANSMIT_TRAY_HANDED_OFF,
} transmit_state_t;

typedef struct {
    bool downstream_detected;
    bool upstream_detected;
} tray_sensor_snapshot_t;

typedef struct {
    statemachine_job_t job;
    QueueHandle_t response_queue;
} statemachine_request_t;

static QueueHandle_t s_job_queue;
static volatile bool s_job_active;
static volatile statemachine_status_t s_status = STATEMACHINE_STATUS_IDLE;

/* Resolves this single-conveyor state machine's configured motor id. */
static esp_err_t get_conveyor_motor_id(const char **out_motor_id)
{
    return hardware_get_motor_id(0, out_motor_id);
}

/* Reads downstream/upstream tray sensors as one protected snapshot. */
static tray_sensor_snapshot_t read_tray_sensors(void)
{
    tray_sensor_snapshot_t snapshot = {0};
    const char *motor_id = NULL;
    int downstream_sensor = !APP_MOTOR_SENSOR_ACTIVE_LEVEL;
    int upstream_sensor = !APP_MOTOR_SENSOR_ACTIVE_LEVEL;

    if (get_conveyor_motor_id(&motor_id) == ESP_OK) {
        (void)hardware_get_sensors(motor_id, &upstream_sensor, &downstream_sensor);
    }

    /* Convert raw GPIO levels into conveyor-specific detected flags. */
    snapshot.downstream_detected = downstream_sensor == APP_MOTOR_SENSOR_ACTIVE_LEVEL;
    snapshot.upstream_detected = upstream_sensor == APP_MOTOR_SENSOR_ACTIVE_LEVEL;
    return snapshot;
}

/* Stops movement when needed and logs the job acknowledgement. */
static void finish_job(statemachine_job_t job, statemachine_result_t result, bool motor_started)
{
    const bool done = result == STATEMACHINE_RESULT_RX_DONE || result == STATEMACHINE_RESULT_TX_DONE;
    const char *motor_id = NULL;
    const char *result_text = "UNKNOWN";

    /* Ask hardware.c to cut conveyor PWM before acknowledging terminal states. */
    if (motor_started && get_conveyor_motor_id(&motor_id) == ESP_OK) {
        stop_motor(motor_id);
    }

    /* Convert the public result enum into a stable acknowledgement token. */
    switch (result) {
    case STATEMACHINE_RESULT_RX_DONE:
        result_text = "RX_DONE";
        break;
    case STATEMACHINE_RESULT_TX_DONE:
        result_text = "TX_DONE";
        break;
    case STATEMACHINE_RESULT_TRAY_ALREADY_PRESENT:
        result_text = "TRAY_ALREADY_PRESENT";
        break;
    case STATEMACHINE_RESULT_TRAY_NOT_RECEIVED:
        result_text = "TRAY_NOT_RECEIVED";
        break;
    case STATEMACHINE_RESULT_TRAY_TRANSFER_STUCK:
        result_text = "TRAY_TRANSFER_STUCK";
        break;
    case STATEMACHINE_RESULT_NO_TRAY_PRESENT:
        result_text = "NO_TRAY_PRESENT";
        break;
    case STATEMACHINE_RESULT_TRAY_HANDOFF_STUCK:
        result_text = "TRAY_HANDOFF_STUCK";
        break;
    case STATEMACHINE_RESULT_EMERGENCY_STOP:
        result_text = "EMERGENCY_STOP";
        break;
    case STATEMACHINE_RESULT_JOB_TIMEOUT:
        result_text = "JOB_TIMEOUT";
        break;
    case STATEMACHINE_RESULT_JOB_REJECTED:
        result_text = "JOB_REJECTED";
        break;
    }

    /* Publish the current acknowledgement as a log until MQTT exists. */
    if (done) {
        ESP_LOGI(TAG, "%s done", job == STATEMACHINE_JOB_RECEIVE ? "jobrx" : "jobtx");
    } else {
        ESP_LOGE(TAG, "%s error=%s", job == STATEMACHINE_JOB_RECEIVE ? "jobrx" : "jobtx", result_text);
    }
}

/* Disables PID ownership and starts raw conveyor movement toward upstream. */
static bool start_moving_upstream(void)
{
    const char *motor_id = NULL;

    if (get_conveyor_motor_id(&motor_id) != ESP_OK) {
        return false;
    }

    /* Prevent pid.c from overwriting this raw state-machine movement. */
    if (pid_set_control(motor_id, false) != ESP_OK) {
        return false;
    }

    /* Ask hardware.c to drive the conveyor toward upstream. */
    return set_motor(motor_id, STATEMACHINE_MOVE_PWM, APP_MOTOR_UPSTREAM_DIRECTION_LEVEL) == ESP_OK;
}

/* Runs one receive job from empty conveyor through upstream arrival. */
static statemachine_result_t run_receive_job(void)
{
    receive_state_t state = RECEIVE_WAITING_FOR_TRAY;
    TickType_t job_start = xTaskGetTickCount();
    TickType_t phase_start = job_start;
    tray_sensor_snapshot_t sensors = {0};
    bool direction_flipped = false;

    /* Use the current runtime travel direction for the whole RX job so entry and arrival sensors stay paired. */
    if (hardware_get_direction_flipped(&direction_flipped) != ESP_OK) {
        finish_job(STATEMACHINE_JOB_RECEIVE, STATEMACHINE_RESULT_JOB_REJECTED, false);
        return STATEMACHINE_RESULT_JOB_REJECTED;
    }

    /* Read initial tray presence before accepting a receive job. */
    s_status = STATEMACHINE_STATUS_RECEIVE_WAITING_FOR_TRAY;
    sensors = read_tray_sensors();
    if (sensors.downstream_detected || sensors.upstream_detected) {
        finish_job(STATEMACHINE_JOB_RECEIVE, STATEMACHINE_RESULT_TRAY_ALREADY_PRESENT, false);
        return STATEMACHINE_RESULT_TRAY_ALREADY_PRESENT;
    }

    /* Run receive states until one state reaches a terminal result. */
    while (true) {
        TickType_t now = xTaskGetTickCount();

        switch (state) {
        case RECEIVE_WAITING_FOR_TRAY:
            s_status = STATEMACHINE_STATUS_RECEIVE_WAITING_FOR_TRAY;
            /* Wait on the entry sensor for the active travel direction. */
            sensors = read_tray_sensors();
            if ((!direction_flipped && sensors.downstream_detected) ||
                (direction_flipped && sensors.upstream_detected)) {
                /* Ask this module's motor helper to begin moving the tray upstream. */
                if (!start_moving_upstream()) {
                    finish_job(STATEMACHINE_JOB_RECEIVE, STATEMACHINE_RESULT_TRAY_TRANSFER_STUCK, true);
                    return STATEMACHINE_RESULT_TRAY_TRANSFER_STUCK;
                }
                phase_start = xTaskGetTickCount();
                state = RECEIVE_MOVING_TRAY;
                break;
            }
            if ((now - job_start) >= pdMS_TO_TICKS(STATEMACHINE_JOB_TIMEOUT_MS)) {
                finish_job(STATEMACHINE_JOB_RECEIVE, STATEMACHINE_RESULT_JOB_TIMEOUT, false);
                return STATEMACHINE_RESULT_JOB_TIMEOUT;
            }
            if ((now - phase_start) >= pdMS_TO_TICKS(STATEMACHINE_RECEIVE_WAIT_TIMEOUT_MS)) {
                finish_job(STATEMACHINE_JOB_RECEIVE, STATEMACHINE_RESULT_TRAY_NOT_RECEIVED, false);
                return STATEMACHINE_RESULT_TRAY_NOT_RECEIVED;
            }
            vTaskDelay(pdMS_TO_TICKS(STATEMACHINE_POLL_MS));
            break;

        case RECEIVE_MOVING_TRAY:
            s_status = STATEMACHINE_STATUS_RECEIVE_MOVING_TRAY;
            /* Stop on the arrival sensor for the same travel direction captured at job start. */
            sensors = read_tray_sensors();
            if ((!direction_flipped && sensors.upstream_detected) ||
                (direction_flipped && sensors.downstream_detected)) {
                state = RECEIVE_TRAY_RECEIVED;
                break;
            }
            if ((now - job_start) >= pdMS_TO_TICKS(STATEMACHINE_JOB_TIMEOUT_MS)) {
                finish_job(STATEMACHINE_JOB_RECEIVE, STATEMACHINE_RESULT_JOB_TIMEOUT, true);
                return STATEMACHINE_RESULT_JOB_TIMEOUT;
            }
            if ((now - phase_start) >= pdMS_TO_TICKS(STATEMACHINE_TRAVEL_TIMEOUT_MS)) {
                finish_job(STATEMACHINE_JOB_RECEIVE, STATEMACHINE_RESULT_TRAY_TRANSFER_STUCK, true);
                return STATEMACHINE_RESULT_TRAY_TRANSFER_STUCK;
            }
            vTaskDelay(pdMS_TO_TICKS(STATEMACHINE_POLL_MS));
            break;

        case RECEIVE_TRAY_RECEIVED:
            s_status = STATEMACHINE_STATUS_RECEIVE_TRAY_RECEIVED;
            /* Acknowledge receive completion through finish_job(). */
            finish_job(STATEMACHINE_JOB_RECEIVE, STATEMACHINE_RESULT_RX_DONE, true);
            return STATEMACHINE_RESULT_RX_DONE;
        }
    }
}

/* Runs one transmit job until the tray fully leaves this conveyor. */
static statemachine_result_t run_transmit_job(void)
{
    transmit_state_t state = TRANSMIT_TRANSMITTING_TRAY;
    TickType_t job_start = xTaskGetTickCount();
    TickType_t phase_start = job_start;
    tray_sensor_snapshot_t sensors = {0};

    /* Read initial tray presence before accepting a transmit job. */
    sensors = read_tray_sensors();
    if (!sensors.downstream_detected && !sensors.upstream_detected) {
        finish_job(STATEMACHINE_JOB_TRANSMIT, STATEMACHINE_RESULT_NO_TRAY_PRESENT, false);
        return STATEMACHINE_RESULT_NO_TRAY_PRESENT;
    }

    /* Ask this module's motor helper to begin handing the tray upstream. */
    if (!start_moving_upstream()) {
        finish_job(STATEMACHINE_JOB_TRANSMIT, STATEMACHINE_RESULT_TRAY_HANDOFF_STUCK, true);
        return STATEMACHINE_RESULT_TRAY_HANDOFF_STUCK;
    }
    phase_start = xTaskGetTickCount();

    /* Run transmit states until one state reaches a terminal result. */
    while (true) {
        TickType_t now = xTaskGetTickCount();

        switch (state) {
        case TRANSMIT_TRANSMITTING_TRAY:
            s_status = STATEMACHINE_STATUS_TRANSMIT_TRANSMITTING_TRAY;
            /* Read sensors while waiting for full upstream handoff. */
            sensors = read_tray_sensors();
            if (!sensors.downstream_detected && !sensors.upstream_detected) {
                state = TRANSMIT_TRAY_HANDED_OFF;
                break;
            }
            if ((now - job_start) >= pdMS_TO_TICKS(STATEMACHINE_JOB_TIMEOUT_MS)) {
                finish_job(STATEMACHINE_JOB_TRANSMIT, STATEMACHINE_RESULT_JOB_TIMEOUT, true);
                return STATEMACHINE_RESULT_JOB_TIMEOUT;
            }
            if ((now - phase_start) >= pdMS_TO_TICKS(STATEMACHINE_HANDOFF_TIMEOUT_MS)) {
                finish_job(STATEMACHINE_JOB_TRANSMIT, STATEMACHINE_RESULT_TRAY_HANDOFF_STUCK, true);
                return STATEMACHINE_RESULT_TRAY_HANDOFF_STUCK;
            }
            vTaskDelay(pdMS_TO_TICKS(STATEMACHINE_POLL_MS));
            break;

        case TRANSMIT_TRAY_HANDED_OFF:
            s_status = STATEMACHINE_STATUS_TRANSMIT_TRAY_HANDED_OFF;
            /* Acknowledge transmit completion through finish_job(). */
            finish_job(STATEMACHINE_JOB_TRANSMIT, STATEMACHINE_RESULT_TX_DONE, true);
            return STATEMACHINE_RESULT_TX_DONE;
        }
    }
}

/* Creates the state-machine job queue before producers can submit work. */
esp_err_t statemachine_init(void)
{
    s_job_queue = xQueueCreate(STATEMACHINE_QUEUE_LEN, sizeof(statemachine_request_t));
    return s_job_queue == NULL ? ESP_ERR_NO_MEM : ESP_OK;
}

/* FreeRTOS task entrypoint that owns all tray job transitions. */
void statemachine_task(void *arg)
{
    statemachine_request_t request = {0};
    statemachine_result_t result = STATEMACHINE_RESULT_JOB_REJECTED;

    (void)arg;

    while (true) {
        s_status = STATEMACHINE_STATUS_IDLE;
        if (s_job_queue == NULL) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (xQueueReceive(s_job_queue, &request, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        s_job_active = true;
        /* Call the requested private job state machine and return its result. */
        result = request.job == STATEMACHINE_JOB_RECEIVE ? run_receive_job() : run_transmit_job();
        if (request.response_queue != NULL) {
            xQueueSend(request.response_queue, &result, portMAX_DELAY);
        }
        s_status = STATEMACHINE_STATUS_IDLE;
        s_job_active = false;
    }
}

/* Queues one receive job and waits for its terminal result. */
statemachine_result_t statemachine_jobrx(void)
{
    statemachine_result_t result = STATEMACHINE_RESULT_JOB_REJECTED;
    statemachine_request_t request = {0};

    if (s_job_queue == NULL) {
        return STATEMACHINE_RESULT_JOB_REJECTED;
    }
    if (s_job_active || uxQueueMessagesWaiting(s_job_queue) > 0) {
        return STATEMACHINE_RESULT_JOB_REJECTED;
    }
    request.job = STATEMACHINE_JOB_RECEIVE;
    request.response_queue = xQueueCreate(1, sizeof(statemachine_result_t));
    if (request.response_queue == NULL) {
        return STATEMACHINE_RESULT_JOB_REJECTED;
    }
    if (xQueueSend(s_job_queue, &request, 0) != pdTRUE) {
        vQueueDelete(request.response_queue);
        return STATEMACHINE_RESULT_JOB_REJECTED;
    }
    xQueueReceive(request.response_queue, &result, portMAX_DELAY);
    vQueueDelete(request.response_queue);
    return result;
}

/* Queues one transmit job and waits for its terminal result. */
statemachine_result_t statemachine_jobtx(void)
{
    statemachine_result_t result = STATEMACHINE_RESULT_JOB_REJECTED;
    statemachine_request_t request = {0};

    if (s_job_queue == NULL) {
        return STATEMACHINE_RESULT_JOB_REJECTED;
    }
    if (s_job_active || uxQueueMessagesWaiting(s_job_queue) > 0) {
        return STATEMACHINE_RESULT_JOB_REJECTED;
    }
    request.job = STATEMACHINE_JOB_TRANSMIT;
    request.response_queue = xQueueCreate(1, sizeof(statemachine_result_t));
    if (request.response_queue == NULL) {
        return STATEMACHINE_RESULT_JOB_REJECTED;
    }
    if (xQueueSend(s_job_queue, &request, 0) != pdTRUE) {
        vQueueDelete(request.response_queue);
        return STATEMACHINE_RESULT_JOB_REJECTED;
    }
    xQueueReceive(request.response_queue, &result, portMAX_DELAY);
    vQueueDelete(request.response_queue);
    return result;
}

/* Returns the current status for callers polling live state-machine progress. */
statemachine_status_t statemachine_get_status(void)
{
    return s_status;
}
