idf_component_register(SRCS
                    "main.c"
                    "app_state.c"
                    "command_task.c"
                    "motor_task.c"
                    "sensor_task.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_driver_gpio esp_driver_ledc microrl)
