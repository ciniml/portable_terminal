#!/usr/bin/env bash
# Build the esp_hosted slave firmware for ESP32-C6 against ESP-IDF 6.0.
# Output: ../managed_components/espressif__esp_hosted/slave/build/network_adapter.bin
# (and bootloader / partition-table / ota_data_initial alongside).
set -euo pipefail

IDF_PATH="${IDF_PATH:-$HOME/esp-idf/6.0}"
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SLAVE_DIR="$PROJECT_ROOT/managed_components/espressif__esp_hosted/slave"

if [[ ! -d "$SLAVE_DIR" ]]; then
    echo "error: $SLAVE_DIR not found." >&2
    echo "Run 'make build' once at the project root so the IDF component" >&2
    echo "manager fetches espressif/esp_hosted." >&2
    exit 1
fi

source "$IDF_PATH/export.sh" >/dev/null
cd "$SLAVE_DIR"

# Patch: the slave's idf_component.yml declares cmd_system as a path
# dependency to IDF's examples/system/console/advanced/components/cmd_system,
# but cmd_system_sleep.c in IDF 6.0 calls esp_sleep_get_wakeup_causes()
# without esp_hw_support in its REQUIRES, so it fails with an implicit
# declaration error. The slave doesn't actually invoke cmd_system —
# strip the dependency so the build stops pulling it in.
MANIFEST="$SLAVE_DIR/main/idf_component.yml"
if grep -q "^  cmd_system:" "$MANIFEST"; then
    echo "Patching slave idf_component.yml: dropping cmd_system dep."
    # Remove the two lines: '  cmd_system:' and the next 'path: ...' line.
    sed -i '/^  cmd_system:/,/^    path:/d' "$MANIFEST"
    # Force component manager re-resolution.
    rm -rf "$SLAVE_DIR/managed_components" "$SLAVE_DIR/dependencies.lock"
fi

idf.py set-target esp32c6
idf.py build

echo
echo "OK. Artefacts:"
echo "  $SLAVE_DIR/build/bootloader/bootloader.bin"
echo "  $SLAVE_DIR/build/partition_table/partition-table.bin"
echo "  $SLAVE_DIR/build/ota_data_initial.bin"
echo "  $SLAVE_DIR/build/network_adapter.bin"
