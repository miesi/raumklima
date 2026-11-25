idf_component_register(SRCS "health.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_system
                       PRIV_REQUIRES espcoredump esp_timer freertos log)
