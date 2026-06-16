idf_component_register(SRCS "sd_event_logger.c"
                    INCLUDE_DIRS "include"
                    REQUIRES fatfs sdmmc esp_driver_spi esp_timer nvs_flash)
