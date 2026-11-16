idf_component_register(SRCS "sensors.c"
                            "drivers/bh1750.c"
                            "drivers/bme68x_bsec.c"
                            "drivers/ds2484.c"
                            "drivers/bme69x.c"
                            "drivers/sht4x.c"
                            "drivers/scd4x.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       REQUIRES esp_driver_i2c onewire
                       PRIV_REQUIRES i2cbus metrics core esp_timer nvs_flash bosch_bme68x)
