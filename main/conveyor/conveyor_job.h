#ifndef CONVEYOR_JOB_H
#define CONVEYOR_JOB_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    CONVEYOR_CMD_START_TX,
    CONVEYOR_CMD_START_RX,
    CONVEYOR_CMD_EMERGENCY_STOP,
    CONVEYOR_CMD_CLEAR_ERROR
} conveyor_cmd_type_t;

typedef enum {
    CONVEYOR_DIR_LEFT,
    CONVEYOR_DIR_RIGHT
} conveyor_direction_t;

typedef enum {
    CONVEYOR_STATE_IDLE,
    CONVEYOR_STATE_TX_WAIT_FOR_TX2_DETECT,
    CONVEYOR_STATE_TX_WAIT_FOR_TX2_CLEAR,
    CONVEYOR_STATE_RX_WAIT_FOR_RX1,
    CONVEYOR_STATE_RX_WAIT_FOR_RX2,
    CONVEYOR_STATE_TX_DONE,
    CONVEYOR_STATE_RX_DONE,
    CONVEYOR_STATE_ERROR,
    CONVEYOR_STATE_ESTOP
} conveyor_state_t;

typedef struct {
    conveyor_cmd_type_t type;
    conveyor_direction_t direction;
} conveyor_cmd_t;

typedef struct {
    conveyor_state_t state;
    conveyor_direction_t direction;
    char error[32];
    int s0;
    int s1;
} conveyor_status_t;

void configure_conveyor_job(void);
void conveyor_job_task(void *arg);
bool conveyor_job_send_command(conveyor_cmd_t command);
bool conveyor_job_is_idle(void);
void conveyor_job_get_status(conveyor_status_t *status);
const char *conveyor_state_name(conveyor_state_t state);
const char *conveyor_direction_name(conveyor_direction_t direction);

#endif
