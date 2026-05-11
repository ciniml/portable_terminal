# Tab5 ESP32-C6 slave firmware (esp_hosted 2.12.7)

Tab5 ships from the factory with the M5Stack-provided
`ESP32C6-WiFi-SDIO-Interface-V1.4.1` slave firmware, which speaks the
**esp_hosted 1.4.x** protocol. ESP-IDF 6.0 only works cleanly with
**esp_hosted 2.x** on the host side, so the C6 has to be re-flashed with
a matching 2.x slave to make Wi-Fi work from this project.

This directory carries the build & flash scripts for that slave.

## Hardware procedure

1. Power off the Tab5.
2. Remove the back cover (the C6 programming header lives inside).
3. Wire an ESP-Prog (or any 3V3 USB-UART adapter that supports DTR/RTS
   auto-reset) to the C6 programming pads:

   | Adapter | C6 pad | Purpose       |
   |---------|--------|---------------|
   | TX      | RXD    | host -> C6    |
   | RX      | TXD    | C6 -> host    |
   | DTR/IO0 | IO0    | enter bootloader |
   | RTS/EN  | EN     | reset         |
   | GND     | GND    |               |
   | (3V3)   | -      | leave open — Tab5 supplies its own |

   (Confirm pad assignments against the Tab5 you have — M5Stack publishes
   the silk on the inner side of the cover.)
4. Power the Tab5 back on with USB-C.
5. Run `./build.sh` once to generate the slave artefacts.
6. Run `./flash.sh /dev/ttyUSBx` (or whichever port the adapter shows up
   on). The script handles bootloader entry via DTR/RTS.
7. Replace the cover and power-cycle the Tab5.

## What gets flashed

`build.sh` configures esp_hosted's slave example for the SDIO transport
expected by our host (4-bit, slot matching the Tab5 PCB routing) and
builds it for `esp32c6`. The artefacts end up under
`../managed_components/espressif__esp_hosted/slave/build/`:

| Offset  | Image                                          |
|---------|------------------------------------------------|
| 0x00000 | `bootloader/bootloader.bin`                    |
| 0x08000 | `partition_table/partition-table.bin`          |
| 0x0d000 | `ota_data_initial.bin`                         |
| 0x10000 | `network_adapter.bin`                          |

`flash.sh` calls `esptool.py write_flash` with these offsets in one shot.

## Once flashed

The host side (this project) will run with `CONFIG_TAB5_WIFI_ENABLED=y`
without further changes. The factory firmware is then gone — re-flash
the M5Stack-provided V1.4.1 from `M5Tab5-UserDemo/platforms/tab5/wifi_c6_fw/`
if you ever need to roll back to the demo's stack (also requires
ESP-IDF 5.4 + esp_hosted 1.4.0 on the host side).
