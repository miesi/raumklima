idf_component_register(SRCS "netmgr.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_netif esp_wifi
                       PRIV_REQUIRES appcfg core esp_timer)
