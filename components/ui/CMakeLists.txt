idf_component_register(SRCS "ui.c"
                            "fonts/font_dejavu_14.c"
                            "fonts/font_dejavu_18.c"
                            "fonts/font_dejavu_bold_24.c"
                            "fonts/font_dejavu_bold_32.c"
                            "fonts/font_dejavu_bold_72.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "fonts"
                       REQUIRES lvgl__lvgl
                       PRIV_REQUIRES epd core appcfg metrics netmgr timesync sensors board console_cmds
                                     esp_timer esp_event esp_app_format esp_system console)
