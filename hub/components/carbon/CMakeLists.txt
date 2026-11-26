idf_component_register(SRCS "carbon.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES appcfg core health knx metrics sensors netmgr timesync netlog lwip
                                     esp_timer esp_system esp_hw_support heap)
