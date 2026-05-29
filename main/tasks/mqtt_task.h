#ifndef MQTT_TASK_H
#define MQTT_TASK_H

#include <stdbool.h>

#include "conveyor_job.h"

void configure_mqtt(void);
void mqtt_status_task(void *arg);
bool mqtt_task_is_connected(void);
void mqtt_publish_job_status(const conveyor_status_t *status);

#endif
