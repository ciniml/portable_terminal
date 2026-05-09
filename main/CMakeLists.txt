idf_component_register(
    SRCS
        "app_main.cpp"
        "display_m5gfx.cpp"
        "input_usb_jtag.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        term_core
        M5Unified
        M5GFX
        esp_driver_usb_serial_jtag
        # carried for later phases (unused yet):
        wireguard
        tailscale
        usb_host_ftdi_sio
)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_23)
