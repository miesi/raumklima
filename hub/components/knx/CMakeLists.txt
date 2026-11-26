idf_component_register(SRCS "knx.c" "knx_proto.c"
                       INCLUDE_DIRS "include"
                       REQUIRES metrics
                       PRIV_REQUIRES appcfg core health netmgr nvs_flash lwip esp_timer esp_hw_support log)
