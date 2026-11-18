idf_component_register(SRCS "netlog.c"
                       INCLUDE_DIRS "include"
                       REQUIRES log
                       PRIV_REQUIRES appcfg core netmgr lwip esp_timer esp_hw_support heap)
