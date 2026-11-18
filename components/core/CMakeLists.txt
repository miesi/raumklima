idf_component_register(SRCS "app_events.c" "sched.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_event
                       PRIV_REQUIRES esp_timer)
