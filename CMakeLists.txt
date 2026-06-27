cmake_minimum_required(VERSION 3.22)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tab5_claude_client)

# -----------------------------------------------------------------------
# c6_fw partition image — auto-update of the ESP32-C6 slave firmware
#
# The Tab5 hosts its Wi-Fi radio on an on-board ESP32-C6 reached over
# SDIO. The slave firmware (network_adapter.bin) is built separately by
# slave_c6_fw/build.sh. We embed the resulting blob into a dedicated
# `c6_fw` data partition (see partitions.csv) so that main/c6_fw_update.cpp
# can stream it to the C6 over esp_hosted RPC at boot when the embedded
# image is strictly newer than the version reported by the running C6.
#
# Two lookup locations, in priority order:
#   1. slave_c6_fw/network_adapter.bin (project-local symlink / copy)
#   2. c6_updater/main/slave_fw/network_adapter.bin (existing standalone-
#      updater convention)
#
# If neither file exists, the build still succeeds — boot-time update
# code logs "no embedded slave fw" and skips the version check.
# -----------------------------------------------------------------------
set(_c6_fw_candidates
    "${CMAKE_SOURCE_DIR}/slave_c6_fw/network_adapter.bin"
    "${CMAKE_SOURCE_DIR}/c6_updater/main/slave_fw/network_adapter.bin"
)
set(_c6_fw_src "")
foreach(_cand ${_c6_fw_candidates})
    if(EXISTS "${_cand}")
        set(_c6_fw_src "${_cand}")
        break()
    endif()
endforeach()

if(_c6_fw_src)
    set(_c6_fw_bin "${CMAKE_BINARY_DIR}/c6_fw.bin")
    # DEPENDS on the source so a touched slave blob retriggers the copy.
    add_custom_command(
        OUTPUT "${_c6_fw_bin}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${_c6_fw_src}" "${_c6_fw_bin}"
        DEPENDS "${_c6_fw_src}"
        COMMENT "Staging c6_fw partition image from ${_c6_fw_src}"
        VERBATIM
    )
    add_custom_target(c6_fw_image ALL DEPENDS "${_c6_fw_bin}")

    # Make `app` (which esptool_py hangs the flash targets off of) wait
    # for the c6_fw image to exist, and register the blob to be flashed
    # at the c6_fw partition's offset. esptool_py_flash_to_partition reads
    # the offset out of the partition table — keep the offset in
    # partitions.csv as the single source of truth.
    add_dependencies(app c6_fw_image)
    esptool_py_flash_to_partition(flash "c6_fw" "${_c6_fw_bin}")
    message(STATUS "c6_fw: embedding slave fw from ${_c6_fw_src}")
else()
    message(STATUS
        "slave_c6_fw blob not found at slave_c6_fw/network_adapter.bin or "
        "c6_updater/main/slave_fw/network_adapter.bin. The c6_fw partition "
        "will remain blank — boot-time auto-update will be skipped. Run "
        "slave_c6_fw/build.sh once to populate the blob.")
endif()
