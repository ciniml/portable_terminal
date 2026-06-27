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
    add_dependencies(app c6_fw_image)

    # c6_fw.bin is an ESP32-C6 application image; esptool inspects each
    # input file's IDF image header and aborts the write_flash with
    # "'c6_fw.bin' is not an ESP32-P4 image" when it sees the C6 chip ID.
    # `--force` is esptool's own escape hatch for this co-processor case
    # (other files in the same invocation still validate normally).
    #
    # We keep the c6_fw write OUT of the main `flash` target — flashing
    # the slave fw isn't needed on every build, and the boot-time C6
    # auto-update (main/c6_fw_update.cpp) carries the blob forward from
    # then on. End users get the partition pre-loaded via the single-
    # file release firmware.bin (pack_firmware.py --extra ...) or the
    # Web Flasher (docs/index.html FLASH_PARTS optional entry).
    #
    # Local developers who want to re-flash the c6_fw partition by hand
    # (e.g. rolling forward to a new slave blob without going through
    # the boot-time RPC) use `make flash-c6` → this target.
    add_custom_target(flash-c6
        COMMAND ${CMAKE_COMMAND} -E echo "Flashing c6_fw partition (--force)"
        COMMAND python -m esptool
                --chip esp32p4
                $<$<BOOL:$ENV{ESPPORT}>:--port> $<$<BOOL:$ENV{ESPPORT}>:$ENV{ESPPORT}>
                --before default-reset --after hard-reset
                write-flash --force
                --flash-mode dio --flash-size 16MB --flash-freq 80m
                0x710000 ${_c6_fw_bin}
        DEPENDS c6_fw_image
        USES_TERMINAL
        VERBATIM
    )

    message(STATUS "c6_fw: embedding slave fw from ${_c6_fw_src}")
else()
    message(STATUS
        "slave_c6_fw blob not found at slave_c6_fw/network_adapter.bin or "
        "c6_updater/main/slave_fw/network_adapter.bin. The c6_fw partition "
        "will remain blank — boot-time auto-update will be skipped. Run "
        "slave_c6_fw/build.sh once to populate the blob.")
endif()
