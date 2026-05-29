idf_component_register(SRCS
                    "main.c"
                    "shared/app_state.c"
                    "tasks/command_task.c"
                    "conveyor/conveyor_job.c"
                    "tasks/motor_task.c"
                    "tasks/mqtt_task.c"
                    "tasks/sensor_task.c"
                    INCLUDE_DIRS "." "config" "shared" "tasks" "conveyor"
                    REQUIRES esp_driver_gpio esp_driver_ledc esp_wifi esp_event esp_netif nvs_flash mqtt microrl)
