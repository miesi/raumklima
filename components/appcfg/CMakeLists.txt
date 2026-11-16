idf_component_register(SRCS "appcfg.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "${CMAKE_CURRENT_BINARY_DIR}"
                       REQUIRES nvs_flash
                       PRIV_REQUIRES esp_hw_support esp_wifi)

# Build-time WLAN defaults from <project>/wlan.config (git-ignored):
#   ssid=<ssid>
#   psk=<pre-shared key>
# Values set on the console (NVS) take precedence. Run "idf.py reconfigure" after
# creating the file for the first time.
idf_build_get_property(project_dir PROJECT_DIR)
set(wlan_cfg "${project_dir}/wlan.config")
set(wlan_ssid "")
set(wlan_psk "")
if(EXISTS "${wlan_cfg}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${wlan_cfg}")
    file(STRINGS "${wlan_cfg}" wlan_lines)
    foreach(line IN LISTS wlan_lines)
        string(REGEX REPLACE "\r$" "" line "${line}")
        if(line MATCHES "^[ \t]*ssid[ \t]*=(.*)$")
            set(wlan_ssid "${CMAKE_MATCH_1}")
        elseif(line MATCHES "^[ \t]*psk[ \t]*=(.*)$")
            set(wlan_psk "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    message(STATUS "wlan.config found, SSID '${wlan_ssid}'")
endif()
foreach(v wlan_ssid wlan_psk)
    string(REPLACE "\\" "\\\\" ${v} "${${v}}")
    string(REPLACE "\"" "\\\"" ${v} "${${v}}")
endforeach()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/wlan_defaults.h.tmp"
     "// generated from wlan.config - do not commit\n"
     "#define WLAN_DEFAULT_SSID \"${wlan_ssid}\"\n"
     "#define WLAN_DEFAULT_PSK \"${wlan_psk}\"\n")
# only touch the header when the content changed, avoids needless rebuilds
file(COPY_FILE "${CMAKE_CURRENT_BINARY_DIR}/wlan_defaults.h.tmp"
     "${CMAKE_CURRENT_BINARY_DIR}/wlan_defaults.h" ONLY_IF_DIFFERENT)
