# Bosch BME68x and BME690 SensorAPI (BSD-3-Clause) and BSEC (closed binary, Bosch license).
# Nothing of it is committed to this repo.
#
# - SensorAPIs and BSEC2: downloaded at configure time from Bosch's GitHub repositories,
#   pinned to a commit and checked against SHA256. BSEC2 supports the BME680 (IAQ) only.
# - BSEC3: if <project>/third_party/bsec3/ holds the unpacked Bosch package (click-through
#   download, git-ignored), BSEC3 is used instead of BSEC2 and gives IAQ for BME680 and
#   BME690. The two cannot be linked together (same symbol names).
#
# By building you accept Bosch's license terms for the BSEC version used:
# BSEC2: https://www.bosch-sensortec.com/media/boschsensortec/downloads/software/bme688_development_software/2024_12/20241219_clickthrough_license_terms_bsec_bme680_bme688_bme690.pdf
# BSEC3: the license shipped with the download.

set(bme68x_rev "80ea120a8b8ac987d7d79eb68a9ed796736be845")   # BME68x_SensorAPI, 2023-05-17 (v4.4.8)
set(bme69x_rev "8a4d9229aaa26c960ca8f7717cb93087f067ecbe")   # BME690_SensorAPI, 2026-02-20
set(bsec2_rev  "4f559a6aa450f0ce436e602b42dc384f52128c66")   # Bosch-BSEC2-Library, 2026-07-08 (1.10.2610)
set(bme68x_url "https://raw.githubusercontent.com/boschsensortec/BME68x_SensorAPI/${bme68x_rev}")
set(bme69x_url "https://raw.githubusercontent.com/boschsensortec/BME690_SensorAPI/${bme69x_rev}")
set(bsec2_url  "https://raw.githubusercontent.com/boschsensortec/Bosch-BSEC2-Library/${bsec2_rev}")
# all configs: 3.3 V supply, 3 s sample rate (LP), 4 days calibration history
set(bsec2_cfg  "src/config/bme680/bme680_iaq_33v_3s_4d/bsec_iaq.txt")
set(bsec3_cfg_680 "bme680/bme680_iaq_33v_3s_4d")
set(bsec3_cfg_690 "bme690/bme690_iaq_33v_3s_4d")

set(dl "${CMAKE_CURRENT_BINARY_DIR}/bosch")
idf_build_get_property(project_dir PROJECT_DIR)
set(bsec3_dir "${project_dir}/third_party/bsec3/release_bin/IAQ")

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
fetch("${bme69x_url}/bme69x.c"       "${dl}/bme69x/bme69x.c"       c079186889087f97b04bf4a7cbceb9366b1ba6e1fa5a2818a24196717fc46faf)
fetch("${bme69x_url}/bme69x.h"       "${dl}/bme69x/bme69x.h"       f8decb821d5bcfd4877e6bfe1e6ff94980055aa6b28aef5a81d8e288cbdb1e60)
fetch("${bme69x_url}/bme69x_defs.h"  "${dl}/bme69x/bme69x_defs.h"  8e0ed7bf0b1f91a5e16012daf99676d1a6ea2d88e6dbd7825be1ae3c45772d49)
fetch("${bme69x_url}/LICENSE"        "${dl}/bme69x/LICENSE"        179c8853c3312def03362077b40d2beeb2f951730622e64987c4d5eef243fef4)

set(srcs "${dl}/bme68x/bme68x.c" "${dl}/bme69x/bme69x.c")
set(incs "${dl}/bme68x" "${dl}/bme69x")

# HUB_FORCE_BSEC2=1 in the environment builds with BSEC2 even if BSEC3 is present (CI, tests)
if(EXISTS "${bsec3_dir}/bin/esp/esp32_s3/libalgobsec.a" AND NOT "$ENV{HUB_FORCE_BSEC2}")
    message(STATUS "BSEC3 found in third_party/bsec3 - IAQ for BME680 and BME690")
    set(bsec_version 3)
    set(bsec_lib "${bsec3_dir}/bin/esp/esp32_s3/libalgobsec.a")
    list(APPEND incs "${bsec3_dir}/inc" "${dl}/bsec3")
    # Bosch ships each config as bsec_iaq.c defining the same symbol; rename per chip.
    foreach(chip 680 690)
        file(READ "${bsec3_dir}/config/${bsec3_cfg_${chip}}/bsec_iaq.c" cfg)
        string(REPLACE "#include \"bsec_iaq.h\"" "#include <stdint.h>" cfg "${cfg}")
        string(REPLACE "bsec_config_iaq" "bsec3_config_bme${chip}" cfg "${cfg}")
        file(WRITE "${dl}/bsec3/bsec3_config_bme${chip}.c.tmp" "${cfg}")
        file(COPY_FILE "${dl}/bsec3/bsec3_config_bme${chip}.c.tmp" "${dl}/bsec3/bsec3_config_bme${chip}.c" ONLY_IF_DIFFERENT)
        list(APPEND srcs "${dl}/bsec3/bsec3_config_bme${chip}.c")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                     "${bsec3_dir}/config/${bsec3_cfg_${chip}}/bsec_iaq.c")
    endforeach()
else()
    message(STATUS "BSEC3 not found - BSEC2 (IAQ for BME680 only)")
    set(bsec_version 2)
    fetch("${bsec2_url}/src/esp32s3/libalgobsec.a" "${dl}/bsec2/libalgobsec.a" 3ed07f5e0bdcb6832de72ff6c6e7ba2b7dcd3ccaa85fd659bce718568d1a34e6)
    fetch("${bsec2_url}/src/inc/bsec_datatypes.h"  "${dl}/bsec2/bsec_datatypes.h" a632c78ca4e38ad7773fe3493ecb2ff2054a0a5eec429c091db9bfe7ed55f927)
    fetch("${bsec2_url}/src/inc/bsec_interface.h"  "${dl}/bsec2/bsec_interface.h" 7643d4dbc0958c9403d5477eb73f9453e54cf69d3ba99cab1c877a0cd4570ae1)
    fetch("${bsec2_url}/${bsec2_cfg}"              "${dl}/bsec2/bsec_iaq.txt"     adf1509b0d07d2eec90d46ac698205de32d899855d95fa054bd2a5464910de6a)
    fetch("${bsec2_url}/LICENSE.md"                "${dl}/bsec2/LICENSE.md"       a27fd42924746a4b45808c21dc71f7c0e6006b4ce9f933d2301e154ed4255d76)
    set(bsec_lib "${dl}/bsec2/libalgobsec.a")
    list(APPEND incs "${dl}/bsec2")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS ${incs})

target_compile_definitions(${COMPONENT_LIB} INTERFACE HUB_BSEC_VERSION=${bsec_version})
add_prebuilt_library(algobsec "${bsec_lib}")
target_link_libraries(${COMPONENT_LIB} INTERFACE algobsec m)
