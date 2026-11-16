idf_component_register(SRCS "onewire.c" "ow_names.c" "ow_sim.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES nvs_flash)
