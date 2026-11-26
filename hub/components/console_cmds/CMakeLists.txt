idf_component_register(SRCS "console_cmds.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES console onewire appcfg health knx carbon netlog netmgr timesync i2cbus sensors metrics board esp_app_format
                                     esp_timer esp_system lwip esp_driver_uart)
