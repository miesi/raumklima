idf_component_register(SRCS "health.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_system
                       PRIV_REQUIRES core espcoredump esp_timer esp_event bootloader_support freertos log)
