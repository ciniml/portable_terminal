#!/usr/bin/env bash
# One-shot updater driver:
#   1. Builds the new esp_hosted 2.x slave (slave_c6_fw/build.sh).
#   2. Copies network_adapter.bin into c6_updater/main/slave_fw/ so the
#      updater app can EMBED_FILES it.
#   3. Builds the updater with IDF 5.4.2 + esp_hosted 1.4.x.
#   4. Flashes the updater to the Tab5 (P4) and tails its log.
#
# Usage:
#   ./updater.sh [<serial-port>]
#
# After the log says "Slave update completed", power-cycle the Tab5 and
# run `make flash monitor` from the project root to reinstall the
# IDF 6.0 main app over the updater.
set -euo pipefail

PORT="${1:-}"
IDF_PATH_5x="${IDF_PATH_5x:-$HOME/esp-idf/5.4.2}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$HERE/.." && pwd)"

# Step 1+2: slave bin
"$PROJECT_ROOT/slave_c6_fw/build.sh"
SLAVE_BIN="$PROJECT_ROOT/managed_components/espressif__esp_hosted/slave/build/network_adapter.bin"
if [[ ! -f "$SLAVE_BIN" ]]; then
    echo "error: slave_c6_fw/build.sh produced no $SLAVE_BIN" >&2
    exit 1
fi
mkdir -p "$HERE/main/slave_fw"
cp "$SLAVE_BIN" "$HERE/main/slave_fw/network_adapter.bin"
echo "Copied $(stat -c %s "$SLAVE_BIN") bytes of slave firmware into updater."

# Step 3: build updater under IDF 5.4
if [[ ! -d "$IDF_PATH_5x" ]]; then
    echo "error: IDF_PATH_5x=$IDF_PATH_5x not found." >&2
    exit 1
fi
source "$IDF_PATH_5x/export.sh" >/dev/null
cd "$HERE"
idf.py set-target esp32p4
idf.py build

# Step 4: flash
if [[ -n "$PORT" ]]; then
    idf.py -p "$PORT" flash monitor
else
    idf.py flash monitor
fi
