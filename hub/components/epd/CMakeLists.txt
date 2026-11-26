idf_component_register(SRCS "epd_ssd1683x2.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES board esp_driver_spi esp_driver_gpio esp_timer)
