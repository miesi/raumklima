# raumklima-hub - ESP-IDF project file
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(raumklima-hub)

# The panic handler arms the RTC watchdog for 10 s (esp_system/port/panic_handler.c) and the
# core dump must be in flash within that time. With CONFIG_LOG_MAXIMUM_LEVEL_DEBUG the core dump
# code prints one debug line per heap block at 115200 baud - seconds - and the watchdog reset
# cut the dump (VERIFIED on the device: "Core dump data check failed" after an
# other_watchdog reset). Info level for that component only.
idf_component_get_property(coredump_lib espcoredump COMPONENT_LIB)
target_compile_definitions(${coredump_lib} PRIVATE LOG_LOCAL_LEVEL=3)
