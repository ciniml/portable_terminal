# tab5_claude_client

M5Stack Tab5 (ESP32-P4) terminal emulator. Long-term goal: multi-connection
client (UART / USB-serial / Telnet / SSH) over WireGuard / Tailscale VPN.

Phase 1 = host-tested VT100 core (`components/term_core`) + Tab5 LCD bring-up.
See `PLAN.md` for the high-level scope and `/home/kenta/.claude/plans/` for the
detailed implementation plan.

## ESP-IDF

Use **ESP-IDF 6.0** at `~/esp-idf/6.0`.

```bash
source ~/esp-idf/6.0/export.sh
```

Default target is `esp32p4`.

## Build / flash

```bash
make set-target          # one-time: idf.py set-target esp32p4
make build               # firmware build for hardware
make flash monitor       # flash to a connected Tab5 and tail logs
```

`build` automatically regenerates `sdkconfig` if the previous run was a QEMU
build (and vice versa) — see the `switch-mode` macro in the Makefile.

## QEMU

```bash
make qemu                # foreground; smoke-test boot only
make qemu-kill           # stop background QEMU instances
```

ESP-IDF 6.0 ESP32-P4 QEMU support is preliminary. Treat `make qemu` as a
boot smoke test only — MIPI-DSI / LCD output is not emulated.

## Host-side unit tests (`term_core`)

The terminal-emulator core is hardware-independent and builds with a system
C++ compiler. GoogleTest is fetched via CMake `FetchContent` on first run.

```bash
make build-host          # cmake + build
make test-host           # ctest
make clean-host
```

Requirements: CMake ≥ 3.22, a C++23-capable compiler (GCC ≥ 13 / Clang ≥ 17).

## Source layout

```
main/                          Target entry: app_main, M5GFX-backed IDisplay
components/term_core/          HW-independent VT100 core (IDF + host dual build)
components/M5GFX/              git submodule — vendored M5GFX, patched for IDF 6.0
components/M5Unified/          git submodule — vendored M5Unified, patched for IDF 6.0
components/wireguard/          From serial_wifi_logger (unused in Phase 1)
components/tailscale/          From serial_wifi_logger (unused in Phase 1)
components/usb_host_ftdi_sio/  From serial_wifi_logger (unused in Phase 1)
host_test/                     Standalone CMake project; gtest unit tests
sdkconfig.defaults*            Project + per-target sdkconfig defaults
partitions.csv                 OTA-friendly partition layout (16 MB flash)
M5_IDF6_PATCHES.md             Notes on local M5GFX/M5Unified IDF 6.0 patches
```

`term_core` exposes a single HAL boundary (`include/term_core/idisplay.hpp`).
On hardware, `main/display_m5gfx.cpp` implements that interface against
M5Unified / M5GFX (Tab5 LCD). In host tests,
`host_test/fakes/fake_display.hpp` records draw calls into a 2D buffer.

## Submodules

`components/M5GFX` and `components/M5Unified` are git submodules. After
`git clone`, run:

```bash
git submodule update --init --recursive
```

Both submodules carry **local patches** (see `M5_IDF6_PATCHES.md`) needed to
build under ESP-IDF 6.0. `git status` will show the submodules as dirty —
this is expected. `git submodule update` overwrites the patches; reapply them
from `M5_IDF6_PATCHES.md` when re-syncing upstream.

## Wi-Fi (ESP32-C6 co-processor)

Tab5's Wi-Fi radio lives on an on-board ESP32-C6 reachable from the P4
over SDIO. We use `espressif/esp_hosted` (^2.12.7) +
`espressif/esp_wifi_remote` (^1.5.1) so that standard `esp_wifi_*` APIs
work transparently against the C6. SDIO pin assignments and bus
parameters come from `sdkconfig.defaults.esp32p4`; PI4IOE2 (0x44) bit 0
is driven HIGH at runtime to power the C6 — see
`main/wifi_setup.cpp::pi4ioe2_init_for_wifi` and `tab5_c6_power_enable`.

**The C6 must be running a slave firmware that matches our esp_hosted
2.x line.** Tab5 ships from the factory with M5Stack's V1.4.1
(`ESP32C6-WiFi-SDIO-Interface-V1.4.1-*.bin`), which speaks esp_hosted
1.4.x — incompatible with our host. Two ways to update the C6:

#### (Preferred) SDIO-only update via the bundled IDF 5.4 updater

`c6_updater/` is a self-contained IDF 5.4.2 project. It speaks
esp_hosted 1.4.0 (factory-compatible), pulls in the SDIO bus the
factory firmware already accepts, and pushes the new 2.x slave we
embed into it via `rpc_ota_begin/write/end`. No cover removal needed.

```bash
./c6_updater/updater.sh /dev/ttyACM0   # build + flash + monitor in one shot
```

Watch the monitor for `Slave update completed`, then reflash the
IDF 6.0 main app over it with `make flash monitor` from the project root.

#### (Fallback) Physical UART flash via ESP-Prog

If the SDIO-only updater fails (e.g. the C6 firmware is non-factory or
broken), open the Tab5 cover and wire an ESP-Prog to the C6 programming
pads per `slave_c6_fw/README.md`:

```bash
./slave_c6_fw/build.sh                 # builds via managed_components/.../slave
./slave_c6_fw/flash.sh /dev/ttyUSB0    # after wiring the adapter
```

Configure SSID / PSK via `idf.py menuconfig` → "Tab5 terminal" → "Wi-Fi".
Wi-Fi is gated on `CONFIG_TAB5_WIFI_ENABLED` (default off) so a fresh
clone still builds and runs without a configured network or a re-flashed
C6.

## Reusable components

`wireguard`, `tailscale`, and `usb_host_ftdi_sio` are vendored from
`/home/kenta/repos/serial_wifi_logger/components/` for use in Phase 2+. They
are present in the IDF build but disabled at runtime in Phase 1
(`CONFIG_TAILSCALE_ENABLE=n`). Re-sync from upstream periodically — there is
no automation for this yet.

`espressif/usb` (transitively required by `usb_host_ftdi_sio`) is pinned to
`==1.1.0` in the FTDI manifest. Newer registry releases break the IDF 6.0
DWC HAL build.
