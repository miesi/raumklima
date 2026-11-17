idf_component_register(SRCS "timesync.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_netif
                       PRIV_REQUIRES appcfg core lwip esp_wifi netmgr)
