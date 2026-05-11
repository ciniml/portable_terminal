#!/usr/bin/env bash
# Flash the freshly-built esp_hosted slave firmware to the Tab5's
# ESP32-C6 via an external USB-UART adapter wired to the C6's
# programming pads (see README.md for the hardware procedure).
#
# Usage:
#   ./flash.sh <serial-port>
#
# Examples:
#   ./flash.sh /dev/ttyUSB0
#   ./flash.sh /dev/cu.usbserial-1410
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: $(basename "$0") <serial-port>" >&2
    echo "  e.g. ./flash.sh /dev/ttyUSB0" >&2
    exit 1
fi
PORT="$1"

IDF_PATH="${IDF_PATH:-$HOME/esp-idf/6.0}"
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SLAVE_BUILD="$PROJECT_ROOT/managed_components/espressif__esp_hosted/slave/build"

for f in \
    "$SLAVE_BUILD/bootloader/bootloader.bin" \
    "$SLAVE_BUILD/partition_table/partition-table.bin" \
    "$SLAVE_BUILD/ota_data_initial.bin" \
    "$SLAVE_BUILD/network_adapter.bin"; do
    if [[ ! -f "$f" ]]; then
        echo "error: $f missing — run ./build.sh first." >&2
        exit 1
    fi
done

source "$IDF_PATH/export.sh" >/dev/null

python -m esptool \
    --chip esp32c6 \
    --port "$PORT" \
    --baud 460800 \
    --before default-reset \
    --after hard-reset \
    write-flash \
    --flash-mode dio \
    --flash-freq 80m \
    --flash-size 4MB \
    0x0     "$SLAVE_BUILD/bootloader/bootloader.bin" \
    0x8000  "$SLAVE_BUILD/partition_table/partition-table.bin" \
    0xd000  "$SLAVE_BUILD/ota_data_initial.bin" \
    0x10000 "$SLAVE_BUILD/network_adapter.bin"

echo
echo "Flashed. Disconnect the adapter, replace the cover, and power-cycle"
echo "the Tab5 via USB-C. Then run 'make flash monitor' on the host side."
