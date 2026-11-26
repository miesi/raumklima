idf_component_register(SRCS "app_main.c"
                       PRIV_REQUIRES board appcfg core health coredumpd netlog carbon netmgr timesync i2cbus metrics sensors console_cmds epd ui onewire
                                     esp_event esp_app_format)
