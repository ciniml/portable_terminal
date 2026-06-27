if(CONFIG_TAB5_BLE_CONFIG_ENABLED)
    idf_component_register(
        SRCS
            src/crypto.cpp
            src/gatt_service.cpp
            src/ble_config.cpp
        INCLUDE_DIRS
            include
        PRIV_INCLUDE_DIRS
            src
        REQUIRES
            mbedtls
        PRIV_REQUIRES
            bt
            nvs_flash
            esp_system
            esp_event
            esp_netif
            esp_timer
            esp_hosted
            main
    )
    target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_23)
    # NimBLE's ble_gatt_chr_def has many fields we don't set (descriptors,
    # cpfd, min_key_size, arg). Partial designated initializers fire
    # -Wmissing-field-initializers under -Werror=extra; relax just for our
    # sources.
    target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)

    # Same MINIMAL_BUILD workaround as for include dirs: link the libraries
    # explicitly because REQUIRES propagation doesn't add them.
    target_link_libraries(${COMPONENT_LIB} PRIVATE
        idf::mbedtls
        idf::bt
        idf::nvs_flash
    )

    # IDF 6.0-beta MINIMAL_BUILD mode does NOT propagate include directories
    # via REQUIRES / PRIV_REQUIRES the way IDF 5 did. Add them explicitly so
    # our crypto / gatt / ble bring-up code can see the bt host headers,
    # mbedtls, nvs_flash etc. Same workaround the wireguard component uses.
    idf_component_get_property(bt_dir         bt         COMPONENT_DIR)
    idf_component_get_property(mbedtls_dir    mbedtls    COMPONENT_DIR)
    idf_component_get_property(nvs_dir        nvs_flash  COMPONENT_DIR)
    idf_component_get_property(esp_system_dir esp_system COMPONENT_DIR)
    idf_component_get_property(esp_event_dir  esp_event  COMPONENT_DIR)
    idf_component_get_property(esp_netif_dir  esp_netif  COMPONENT_DIR)
    idf_component_get_property(esp_timer_dir  esp_timer  COMPONENT_DIR)
    idf_component_get_property(esp_partition_dir esp_partition COMPONENT_DIR)
    idf_component_get_property(esp_blockdev_dir  esp_blockdev  COMPONENT_DIR)
    idf_component_get_property(main_dir       main       COMPONENT_DIR)
    target_include_directories(${COMPONENT_LIB} PRIVATE
        # bt: NimBLE host headers (host/ble_*.h, services/gap/, services/gatt/,
        # nimble_port.h, nimble_port_freertos.h).
        ${bt_dir}/host/nimble/nimble/nimble/host/include
        ${bt_dir}/host/nimble/nimble/nimble/include
        ${bt_dir}/host/nimble/nimble/nimble/host/services/gap/include
        ${bt_dir}/host/nimble/nimble/nimble/host/services/gatt/include
        ${bt_dir}/host/nimble/nimble/nimble/host/util/include
        ${bt_dir}/host/nimble/nimble/nimble/host/store/config/include
        ${bt_dir}/host/nimble/nimble/porting/nimble/include
        ${bt_dir}/host/nimble/port/include
        ${bt_dir}/host/nimble/nimble/nimble/transport/include
        # NimBLE FreeRTOS port (nimble_npl_os.h etc.) — the two npl include
        # roots that ship in IDF 6.0; the second is the canonical path,
        # the first is the legacy mirror still present in the bt tree.
        ${bt_dir}/porting/npl/freertos/include
        ${bt_dir}/host/nimble/nimble/porting/npl/freertos/include
        ${bt_dir}/host/nimble/nimble/porting/nimble/include
        # mbedtls
        ${mbedtls_dir}/port/include
        ${mbedtls_dir}/mbedtls/include
        # nvs_flash + esp_partition + esp_blockdev (transitive include chain
        # from nvs_flash.h → esp_partition.h → esp_blockdev.h).
        ${nvs_dir}/include
        ${esp_partition_dir}/include
        ${esp_blockdev_dir}/include
        # misc
        ${esp_system_dir}/include
        ${esp_event_dir}/include
        ${esp_netif_dir}/include
        ${esp_timer_dir}/include
        # main: profiles.hpp + wifi_config.hpp
        ${main_dir}
    )
else()
    # Register empty component when not selected so the build still resolves.
    idf_component_register()
endif()
