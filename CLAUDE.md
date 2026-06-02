# tab5_claude_client

M5Stack Tab5 (ESP32-P4) terminal emulator. Long-term goal: multi-connection
client (UART / USB-serial / Telnet / SSH) over WireGuard / Tailscale VPN.

Phase 1 = host-tested VT100 core (`components/term_core`) + Tab5 LCD bring-up.
See `PLAN.md` for the high-level scope.

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
components/libssh2/            libssh2 1.11.1 submodule + custom CMakeLists (mbedTLS)
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

`components/M5GFX`, `components/M5Unified`, and `components/libssh2/libssh2`
are git submodules. After `git clone`, run:

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

## Remote connection abstraction

[main/connection.hpp](main/connection.hpp) defines `IConnection`
(`start / stop / send / resize / is_connected / kind / host_label`),
the uniform surface used by `app_main`, the status panel, and the
soft-keyboard resize plumbing. A single active connection at a time
is tracked via `tab5::active_connection()` (MVP — multi-session is
future work).

Two concrete implementations ship:

### SSH

When `CONFIG_TAB5_SSH_ENABLED=y` and Wi-Fi has come up, `app_main`
opens an interactive shell session to the configured server via
[main/ssh_connection.{hpp,cpp}](main/ssh_connection.cpp), built on
libssh2 1.11.1 + mbedTLS (`LIBSSH2_MBEDTLS=1`). Configure host /
port / user / password via `idf.py menuconfig` → "Tab5 terminal"
→ "SSH" or in `sdkconfig.defaults.local`.

#### Public-key authentication

Set `CONFIG_TAB5_SSH_PUBKEY_AUTH=y` and drop a PEM RSA private key
at [main/keys/id_rsa](main/keys/) (the `main/keys/` directory is
gitignored — do not commit private keys). The build embeds the key
via `EMBED_FILES`; the runtime calls
`libssh2_userauth_publickey_frommemory` and falls back to password
authentication if it fails. libssh2 derives the public key from the
private one, so a `.pub` file isn't strictly needed in the firmware
— you only need it on the server side via `ssh-copy-id`. Generate:

```bash
ssh-keygen -m PEM -t rsa -b 2048 -f main/keys/id_rsa
ssh-copy-id -i main/keys/id_rsa.pub user@host
```

PEM RSA / EC keys are the supported formats. New-format OpenSSH
(`-----BEGIN OPENSSH PRIVATE KEY-----`, including ed25519) won't
load under libssh2 + mbedTLS without additional patches.

### Telnet

When SSH is disabled (or its `start()` failed) and
`CONFIG_TAB5_TELNET_ENABLED=y`, `app_main` falls back to plain TCP +
RFC 854 telnet via
[main/telnet_connection.{hpp,cpp}](main/telnet_connection.cpp). The
implementation strips IAC option negotiations from the RX stream
(refusing every DO/WILL with DONT/WONT, except DO NAWS which is
accepted), escapes literal 0xFF on TX, and reports window-size
changes through NAWS subnegotiation.

While a remote session is up, the input sinks bypass the
`CookedInputFilter` — the remote handles CR/BS itself. If start
fails, sinks fall back to the local-echo terminal so the device
stays usable as a status display.

### Auto-reconnect

[main/reconnect.{hpp,cpp}](main/reconnect.cpp) spawns a supervisor
task that watches `IConnection::is_connected()`. On a drop it
waits with exponential backoff (1 s → 30 s) and calls `start()`
again on the same connection object, then replays the current
pty size via `resize()`. The supervisor outlives the boot path
and handles transient Wi-Fi outages naturally (dial() inside
start() just fails fast while Wi-Fi is down).

Dead-link detection uses two complementary mechanisms:

- TCP keepalive on the underlying socket — `SO_KEEPALIVE` with
  `TCP_KEEPIDLE=30 s`, `TCP_KEEPINTVL=10 s`, `TCP_KEEPCNT=3` →
  a stale remote / Wi-Fi outage surfaces as `recv` failure in
  ~60 s.
- SSH-level keepalive — `libssh2_keepalive_config` with a 30 s
  interval; `ssh_task` calls `libssh2_keepalive_send` once a
  second to actually emit packets (libssh2 doesn't drive them
  on its own).

The race between a dying I/O task tearing down its session and
the supervisor's next `start()` is guarded by an internal
`task_alive` atomic; `start()` refuses until the previous task
fully exits.

**Host-key verification: TOFU (Trust On First Use).** First connect to
a given host:port records SHA-256 of `libssh2_session_hostkey()` in NVS
namespace `"ssh_tofu"` (key is the 32-bit FNV-1a hash of "host:port",
hex-encoded — NVS key length limit is 15 chars). Subsequent connects
require an exact match or the session is refused. To re-trust a host
(e.g. after a server rekey), `idf.py erase-flash` clears all
fingerprints; selectively rotating one entry isn't exposed yet.

## On-device peripherals

- **Status panel** ([main/status_bar.cpp](main/status_bar.cpp)) — renders into
  the 160 px right margin (terminal grid is 80×30 × 12×24 = 960×720 on a
  1280×720 screen, so margin space is free). Shows battery %, charging,
  Wi-Fi IP, SSH state, uptime. Refresh every 5 s through the caller-supplied
  lock callback so all M5GFX access stays serialised with the terminal.
- **Buzzer** — BEL (0x07) is wired through `IDisplay::bell()` (default no-op
  on the host fake); [display_m5gfx.cpp](main/display_m5gfx.cpp) implements
  it via `M5.Speaker.tone(880, 60)`. Audio path goes through M5Unified's
  I2S codec wrapper — see [M5_IDF6_PATCHES.md](M5_IDF6_PATCHES.md) for the
  IDF 6 audio status (compiles, runtime best-effort).
- **Touch input** ([main/input_touch.cpp](main/input_touch.cpp)) — a 60 Hz
  polling task drives `M5.update()` and emits `Down/Move/Up` edge events
  to a `TouchSink`. GT911 panel via M5GFX. The events go to ESP_LOGI for
  now; a soft-keyboard UI on top is a follow-up.

## Reusable components

`wireguard`, `tailscale`, and `usb_host_ftdi_sio` are vendored from
[ciniml/serial_wifi_logger](https://github.com/ciniml/serial_wifi_logger)
(`components/`) for use in Phase 2+. They are present in the IDF build but
disabled at runtime in Phase 1 (`CONFIG_TAILSCALE_ENABLE=n`). Re-sync from
upstream periodically — there is no automation for this yet.

`espressif/usb` (transitively required by `usb_host_ftdi_sio`) is pinned to
`==1.1.0` in the FTDI manifest. Newer registry releases break the IDF 6.0
DWC HAL build.
