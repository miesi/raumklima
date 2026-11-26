idf_component_register(SRCS "coredumpd.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES core health espcoredump esp_http_server esp_partition esp_event esp_timer
                                     lwip log)
