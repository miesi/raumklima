idf_component_register(SRCS "netlog.c"
                       INCLUDE_DIRS "include"
                       REQUIRES log
                       PRIV_REQUIRES appcfg core health netmgr lwip esp_timer esp_hw_support heap)
