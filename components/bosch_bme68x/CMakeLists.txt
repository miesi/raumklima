# Bosch BME68x SensorAPI (BSD-3-Clause) and BSEC2 (closed binary, Bosch license).
# Neither is committed to this repo. Both are downloaded at configure time from Bosch's
# GitHub repositories, pinned to a commit and checked against SHA256.
#
# By building with BSEC2 you accept Bosch's license terms for BSEC 2.x:
# https://www.bosch-sensortec.com/media/boschsensortec/downloads/software/bme688_development_software/2024_12/20241219_clickthrough_license_terms_bsec_bme680_bme688_bme690.pdf

set(bme68x_rev "80ea120a8b8ac987d7d79eb68a9ed796736be845")   # BME68x_SensorAPI, 2023-05-17 (v4.4.8)
set(bsec2_rev  "4f559a6aa450f0ce436e602b42dc384f52128c66")   # Bosch-BSEC2-Library, 2026-07-08 (1.10.2610)
set(bme68x_url "https://raw.githubusercontent.com/boschsensortec/BME68x_SensorAPI/${bme68x_rev}")
set(bsec2_url  "https://raw.githubusercontent.com/boschsensortec/Bosch-BSEC2-Library/${bsec2_rev}")
# 3.3 V supply, 3 s sample rate (LP), 4 days calibration history
set(bsec2_cfg  "src/config/bme680/bme680_iaq_33v_3s_4d/bsec_iaq.txt")

set(dl "${CMAKE_CURRENT_BINARY_DIR}/bosch")

function(fetch url dst sha256)
    if(EXISTS "${dst}")
        file(SHA256 "${dst}" have)
        if(have STREQUAL sha256)
            return()
        endif()
    endif()
    message(STATUS "downloading ${url}")
    file(DOWNLOAD "${url}" "${dst}" EXPECTED_HASH SHA256=${sha256} STATUS st TLS_VERIFY ON)
    list(GET st 0 rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "download failed: ${url}: ${st}")
    endif()
endfunction()

fetch("${bme68x_url}/bme68x.c"       "${dl}/bme68x/bme68x.c"       bad525ea57fe57a5d7dda19d287781bcbab50fdbf2dedea737c16b206cbe0157)
fetch("${bme68x_url}/bme68x.h"       "${dl}/bme68x/bme68x.h"       09ed6babf92955ced360790958e2e2f03efb85bc4f0a8a864a4151a905eaf0ca)
fetch("${bme68x_url}/bme68x_defs.h"  "${dl}/bme68x/bme68x_defs.h"  5f59c0bafbcc416c091a1a1cc40bb7362f0b48ea02ac7fa5197813f491de01aa)
fetch("${bme68x_url}/LICENSE"        "${dl}/bme68x/LICENSE"        56df116398e0b72a12ae353d93002d29175583590283ced2c2510e3d8075546f)
fetch("${bsec2_url}/src/esp32s3/libalgobsec.a" "${dl}/bsec2/libalgobsec.a" 3ed07f5e0bdcb6832de72ff6c6e7ba2b7dcd3ccaa85fd659bce718568d1a34e6)
fetch("${bsec2_url}/src/inc/bsec_datatypes.h"  "${dl}/bsec2/bsec_datatypes.h" a632c78ca4e38ad7773fe3493ecb2ff2054a0a5eec429c091db9bfe7ed55f927)
fetch("${bsec2_url}/src/inc/bsec_interface.h"  "${dl}/bsec2/bsec_interface.h" 7643d4dbc0958c9403d5477eb73f9453e54cf69d3ba99cab1c877a0cd4570ae1)
fetch("${bsec2_url}/${bsec2_cfg}"              "${dl}/bsec2/bsec_iaq.txt"     adf1509b0d07d2eec90d46ac698205de32d899855d95fa054bd2a5464910de6a)
fetch("${bsec2_url}/LICENSE.md"                "${dl}/bsec2/LICENSE.md"       a27fd42924746a4b45808c21dc71f7c0e6006b4ce9f933d2301e154ed4255d76)

idf_component_register(SRCS "${dl}/bme68x/bme68x.c"
                       INCLUDE_DIRS "${dl}/bme68x" "${dl}/bsec2")

add_prebuilt_library(algobsec "${dl}/bsec2/libalgobsec.a")
target_link_libraries(${COMPONENT_LIB} INTERFACE algobsec)
# libalgobsec needs libm
target_link_libraries(${COMPONENT_LIB} INTERFACE m)
