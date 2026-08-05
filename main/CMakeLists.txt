set(_embed_txtfiles "")
if(CONFIG_TAB5_SSH_PUBKEY_AUTH)
    set(_privkey "${CMAKE_CURRENT_SOURCE_DIR}/keys/id_rsa")
    if(NOT EXISTS "${_privkey}")
        message(FATAL_ERROR
            "CONFIG_TAB5_SSH_PUBKEY_AUTH=y but ${_privkey} is missing. "
            "Generate one with:\n"
            "  ssh-keygen -m PEM -t rsa -b 2048 -f main/keys/id_rsa\n"
            "then ssh-copy-id the matching .pub to the remote.")
    endif()
    # TXTFILES (vs FILES) appends a NUL byte to the embedded blob — mbedTLS
    # 3.x's mbedtls_pk_parse_key requires PEM input to be NUL-terminated,
    # and silently returns -1 if it isn't.
    list(APPEND _embed_txtfiles "keys/id_rsa")
endif()

if(CONFIG_TAB5_HTTP_CONFIG_ENABLED)
    # Settings landing page served at GET /. TXTFILES appends a NUL so
    # the handler can treat the blob as a C string.
    list(APPEND _embed_txtfiles "http_config_page.html")
endif()

idf_component_register(
    SRCS
        "app_main.cpp"
        "ap_qr_screen.cpp"
        "boot_progress.cpp"
        "captive_portal.cpp"
        "cooked_input.cpp"
        "cursor_renderer.cpp"
        "display_m5gfx.cpp"
        "hid_translate.cpp"
        "http_config.cpp"
        "input_tab5_kbd.cpp"
        "input_usb_jtag.cpp"
        "input_uart.cpp"
        "input_touch.cpp"
        "input_usb_hid.cpp"
        "menu.cpp"
        "ota_task.cpp"
        "soft_keyboard.cpp"
        "wifi_config.cpp"
        "wifi_setup.cpp"
        "connection.cpp"
        "profiles.cpp"
        "reconnect.cpp"
        "ssh_connection.cpp"
        "telnet_connection.cpp"
        "tofu.cpp"
        "usb_host_root.cpp"
        "usbserial_connection.cpp"
        "vpn.cpp"
        "status_bar.cpp"
        "ui_root.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        term_core
        M5Unified
        M5GFX
        esp_driver_usb_serial_jtag
        esp_driver_uart
        esp_timer
        esp_wifi
        esp_event
        esp_netif
        nvs_flash
        libssh2
        mbedtls
        lwip
        app_update
        esp_http_client
        esp_http_server
        esp_https_ota
        espressif__cjson
        espressif__usb
        espressif__usb_host_hid
        # esp_driver_i2c is needed by main/input_tab5_kbd.cpp's standalone
        # I2C driver for the Tab5 clip-on keyboard.
        esp_driver_i2c
        esp_driver_gpio
        # carried for later phases (unused yet):
        wireguard
        tailscale
        usb_host_ftdi_sio
    EMBED_TXTFILES ${_embed_txtfiles}
)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_23)
