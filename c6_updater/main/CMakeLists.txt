# The new 2.x slave network_adapter.bin is copied here by
# ../updater.sh before the build, so we can EMBED_FILES it into the
# app image with no separate flash partition.
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    EMBED_FILES "slave_fw/network_adapter.bin"
    PRIV_REQUIRES
        espressif__esp_hosted
        esp_driver_i2c
        esp_driver_gpio
        # esp_hosted 1.4.0 leaks these headers out of its public API and
        # doesn't propagate the matching REQUIRES — declare them here so
        # they're visible to main and to the esp_hosted compile units.
        esp_wifi
        esp_driver_sdmmc
)
