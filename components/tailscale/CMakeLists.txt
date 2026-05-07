if(CONFIG_TAILSCALE_ENABLE)
    idf_component_register(
        SRCS
            "src/tailscale_esp32.c"
            "src/tailscale_keys.c"
            "src/tailscale_noise.c"
            "src/tailscale_control.c"
            "src/tailscale_netmap.c"
            "src/tailscale_derp.c"
            "src/tailscale_disco.c"
            "src/tailscale_jstream.c"
        INCLUDE_DIRS
            "include"
        PRIV_INCLUDE_DIRS
            "src"
        REQUIRES
            wireguard
            esp-tls
        PRIV_REQUIRES
            esp_netif
            lwip
            nvs_flash
            espressif__cjson
            esp_system
            mbedtls
    )

    # Explicitly pull in include paths that may not propagate in MINIMAL_BUILD.
    idf_component_get_property(esp_tls_dir       esp-tls          COMPONENT_DIR)
    idf_component_get_property(mbedtls_dir       mbedtls          COMPONENT_DIR)
    idf_component_get_property(nvs_flash_dir     nvs_flash        COMPONENT_DIR)
    idf_component_get_property(cjson_dir         espressif__cjson COMPONENT_DIR)
    idf_component_get_property(wireguard_dir     wireguard        COMPONENT_DIR)
    idf_component_get_property(esp_partition_dir esp_partition    COMPONENT_DIR)
    idf_component_get_property(esp_netif_dir     esp_netif        COMPONENT_DIR)
    idf_component_get_property(esp_blockdev_dir  esp_blockdev     COMPONENT_DIR)
    target_include_directories(${COMPONENT_LIB} PRIVATE
        ${esp_tls_dir}
        ${mbedtls_dir}/mbedtls/include
        ${mbedtls_dir}/mbedtls/tf-psa-crypto/include
        ${mbedtls_dir}/mbedtls/tf-psa-crypto/drivers/builtin/include
        ${mbedtls_dir}/port/include
        ${mbedtls_dir}/esp_crt_bundle/include
        ${nvs_flash_dir}/include
        ${cjson_dir}/cJSON
        ${wireguard_dir}/include
        ${wireguard_dir}/src
        ${wireguard_dir}/src/crypto/refc
        ${esp_partition_dir}/include
        ${esp_blockdev_dir}/include
        ${esp_netif_dir}/include
    )

    # Ensure wireguard crypto symbols are not dropped by --gc-sections.
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--undefined=wireguard_esp32_start_managed")
else()
    idf_component_register()
endif()
