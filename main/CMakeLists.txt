idf_component_register(SRCS "app_main.c"
                       PRIV_REQUIRES board appcfg core netmgr timesync i2cbus metrics sensors console_cmds
                                     esp_event esp_app_format)
