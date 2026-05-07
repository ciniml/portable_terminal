idf_component_register(
    SRCS "app_main.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        term_core
        # carried for later phases (unused in Phase 1):
        wireguard
        tailscale
        usb_host_ftdi_sio
)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_23)
