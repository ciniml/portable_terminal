# tab5_claude_client

Terminal emulator for **M5Stack Tab5** (ESP32-P4, 1280×720 MIPI-DSI LCD). Supports
SSH and Telnet over Wi-Fi, with WireGuard / Tailscale VPN for reaching hosts
behind NAT. Designed to be usable as a portable hardware SSH terminal that
talks directly to your home / lab servers via Tailscale, with NAT traversal
to avoid renting an always-on relay machine.

A Japanese version of this README is at [README.ja.md](README.ja.md).

---

## Features

- **80×30 VT100 terminal** on the Tab5 LCD via [M5GFX](https://github.com/m5stack/M5GFX) /
  [M5Unified](https://github.com/m5stack/M5Unified)
- **Remote connections**
  - SSH (libssh2 + mbedTLS, password and embedded-key auth, TOFU host-key verification)
  - Telnet (RFC 854 + NAWS)
  - Auto-reconnect with exponential backoff + TCP/SSH keepalive
- **Tailscale client** (control plane, DERP, DISCO, NAT traversal) — see
  [docs/TAILSCALE_PORTING_NOTES.md](docs/TAILSCALE_PORTING_NOTES.md) for the full
  protocol notes
  - DISCO Ping/Pong direct-path verification
  - DISCO CallMeMaybe (send & receive)
  - HTTPS-based public-IP probe (replaces UDP STUN for MAP-E / IPv4-over-IPv6
    home networks where STUN responses are silently dropped)
  - WireGuard data plane with managed mode + DERP fallback
- **Input**
  - Touch screen (60 Hz polling)
  - On-screen soft keyboard with terminal-side resize plumbing
  - Optional clip-on **Tab5 Keyboard** (I2C HID, software typematic / auto-repeat)
- **Status panel** — battery, charging, Wi-Fi IP, SSH state, uptime
- **Buzzer** (BEL via M5.Speaker)
- **Host-side unit tests** for the hardware-independent VT100 core (52 tests via
  GoogleTest)

## Hardware

- **M5Stack Tab5** (ESP32-P4 + on-board ESP32-C6 for Wi-Fi via SDIO)
- Optionally: **Tab5 Keyboard** (clip-on QWERTY, I2C 0x6D on ExtPort1)

## Repository layout

```
main/                          Target entry: app_main, M5GFX-backed IDisplay
components/term_core/          HW-independent VT100 core (IDF + host dual build)
components/M5GFX/              git submodule — vendored, patched for IDF 6.0
components/M5Unified/          git submodule — vendored, patched for IDF 6.0
components/libssh2/            libssh2 1.11.1 submodule + custom CMake wrapper (mbedTLS)
components/wireguard/          From serial_wifi_logger
components/tailscale/          From serial_wifi_logger, extended substantially
components/usb_host_ftdi_sio/  From serial_wifi_logger (used in Phase 2+)
host_test/                     Standalone CMake project; gtest unit tests
sdkconfig.defaults*            Project + per-target sdkconfig defaults
partitions.csv                 OTA-friendly partition layout (16 MB flash)
c6_updater/                    Standalone IDF 5.4 project to update C6 firmware over SDIO
slave_c6_fw/                   C6 slave firmware (esp_hosted 2.x)
docs/                          Documentation
M5_IDF6_PATCHES.md             Notes on local M5GFX/M5Unified IDF 6.0 patches
```

## Quick start — flash a pre-built release

The fastest way to try the firmware is the **Web Flasher**:

<https://ciniml.github.io/portable_terminal/>

Connect your Tab5 via USB, pick a tagged release, and click Flash. Requires
Chrome / Edge with WebSerial support. Note that the C6 needs to already be
running compatible firmware — see "Wi-Fi C6 firmware" below if Wi-Fi isn't
needed yet you can still flash the main app and use UART input.

Released artifacts are also downloadable from
[GitHub Releases](https://github.com/ciniml/portable_terminal/releases) — each
release ZIP contains a single packed `firmware-vX.Y.Z.bin` plus the individual
`bootloader.bin` / `partition-table.bin` / `ota_data_initial.bin` / `tab5_claude_client.bin`
for use with esptool.

## Prerequisites (for building from source)

- **ESP-IDF 6.0** at `~/esp-idf/6.0`
  ```bash
  source ~/esp-idf/6.0/export.sh
  ```
- Compatible C6 slave firmware on the Tab5 (see "Wi-Fi C6 firmware" below)
- For host tests: CMake ≥ 3.22, GCC ≥ 13 or Clang ≥ 17 (C++23)

## Building and flashing from source

```bash
git clone --recursive git@github.com:ciniml/portable_terminal.git
cd portable_terminal

make set-target          # one-time: idf.py set-target esp32p4
make build               # firmware build for hardware
make flash monitor       # flash to a connected Tab5 and tail logs
```

If submodules are missing:

```bash
git submodule update --init --recursive
```

Both `components/M5GFX` and `components/M5Unified` carry local IDF 6.0 patches —
see [M5_IDF6_PATCHES.md](M5_IDF6_PATCHES.md). `git status` showing the submodules
as dirty is expected; if you re-sync upstream, reapply the patches.

## Configuration

Most settings live in `sdkconfig.defaults` and are exposed via Kconfig under
`Tab5 terminal`. The easiest path is to copy the example file and edit it:

```bash
cp sdkconfig.defaults.local.example sdkconfig.defaults.local
$EDITOR sdkconfig.defaults.local
make build               # picks up sdkconfig.defaults.local
```

Key options:

| Option | Purpose |
|---|---|
| `CONFIG_TAB5_WIFI_ENABLED` | Bring up Wi-Fi via the C6 co-processor |
| `CONFIG_TAB5_WIFI_SSID` / `_PASSWORD` | Wi-Fi credentials |
| `CONFIG_TAB5_SSH_ENABLED` | Enable SSH connection on boot |
| `CONFIG_TAB5_SSH_HOST` / `_PORT` / `_USER` / `_PASSWORD` | SSH endpoint |
| `CONFIG_TAB5_SSH_PUBKEY_AUTH` | Use embedded `main/keys/id_rsa` (PEM) instead of password |
| `CONFIG_TAB5_TELNET_ENABLED` | Telnet fallback when SSH disabled / fails |
| `CONFIG_TAILSCALE_ENABLE` | Compile in the Tailscale stack |
| `CONFIG_TAILSCALE_AUTH_KEY` | Tailscale auth key (`tskey-auth-…`) |
| `CONFIG_TAILSCALE_HOSTNAME` | Hostname advertised to the tailnet |
| `CONFIG_TAILSCALE_LISTEN_PORT` | WireGuard UDP listen port (default 41641) |

For SSH public-key authentication:

```bash
ssh-keygen -m PEM -t rsa -b 2048 -f main/keys/id_rsa
ssh-copy-id -i main/keys/id_rsa.pub user@host
```

`main/keys/` is gitignored — do **not** commit private keys.

## Wi-Fi C6 firmware

Tab5 ships with an old `esp_hosted` 1.4.x slave firmware on the on-board C6.
This project requires the 2.x line. Two ways to update:

**Preferred — SDIO-only update via the bundled IDF 5.4 updater** (no cover removal):

```bash
./c6_updater/updater.sh /dev/ttyACM0   # build + flash + monitor in one shot
```

Wait for `Slave update completed`, then reflash this main app with
`make flash monitor`.

**Fallback — physical UART flash via ESP-Prog**:

Open the Tab5 cover, wire an ESP-Prog to the C6 programming pads
(see [slave_c6_fw/README.md](slave_c6_fw/README.md)), then:

```bash
./slave_c6_fw/build.sh
./slave_c6_fw/flash.sh /dev/ttyUSB0
```

## Usage

After flashing:
1. Tab5 boots and brings up Wi-Fi (status shown on the right panel).
2. If `CONFIG_TAILSCALE_ENABLE` is set, the Tailscale client comes up. First
   boot requires authentication — the auth URL is displayed in the log and on
   the status panel.
3. If `CONFIG_TAB5_SSH_ENABLED` is set, the configured SSH session opens
   automatically.
4. Disconnect / reconnect is handled by the supervisor task with exponential
   backoff (1 s → 30 s).

The touch screen drives a soft keyboard; the optional clip-on Tab5 Keyboard
gives a hardware QWERTY. Both feed into the same terminal sink.

## Host-side tests

The `term_core` library is hardware-independent and builds with the system C++
compiler. GoogleTest is fetched via CMake `FetchContent` on first run.

```bash
make build-host
make test-host
make clean-host
```

Expected: 52 / 52 tests pass.

## HTTP settings service (softAP + QR onboarding)

With `CONFIG_TAB5_HTTP_CONFIG_ENABLED=y` (default) the device brings up a
WPA2 softAP `Tab5-XXXXXX` (suffix = AP MAC) alongside the STA connection and
serves a settings page at `http://192.168.4.1/` plus a JSON status API at
`/api/info`.

- **Per-device AP password** — generated once on first boot (10 chars,
  lowercase + digits without the ambiguous `0/o/1/l`) and persisted in NVS
  (namespace `httpcfg`, key `ap_psk`), so it is stable across reboots.
  Setting a non-empty `CONFIG_TAB5_HTTP_CONFIG_AP_PSK` overrides it (dev
  convenience). Forgot the password? It is printed in the terminal boot
  output (`Config AP "Tab5-XXXXXX" pass "..."`), and `idf.py erase-flash`
  regenerates it.
- **QR onboarding** — when the STA connect *fails* (wrong / stale
  credentials), the LCD shows a full-screen overlay with two QR codes: a
  Wi-Fi QR (`WIFI:T:WPA;...`, joins the phone to the AP straight from the
  stock camera app) and a URL QR for `http://192.168.4.1/`, plus the
  credentials in plain text. Any key press or tap dismisses it.
- **Captive portal** — a tiny UDP/53 DNS responder answers every query with
  `192.168.4.1`, and the HTTP server 302-redirects the OS connectivity
  probes (`/generate_204`, `/hotspot-detect.html`, `/connecttest.txt`, ...)
  and all unknown paths to the settings page — so most phones pop the
  portal automatically right after joining the AP.

## OTA update

The firmware polls a `latest.json` manifest on GitHub Pages every 6 h
(±15 min jitter, seeded from the STA MAC) and installs newer builds via
`esp_https_ota`. The pipeline is:

1. GET `<base>/latest.json` (default base URL is a Kconfig value; NVS
   `ota.base_url` overrides).
2. SemVer-compare `esp_app_desc.version` against `latest.json:tag`.
   Downgrades are refused unless the manifest sets `allow_downgrade: true`.
3. Download the referenced app image (256 KB partial-HTTP chunks so a
   Wi-Fi flap doesn't cost a full restart).
4. Deferred reboot — wait until no `IConnection` is active (up to 30 min).
5. After the new image boots, wait 120 s of healthy operation and call
   `esp_ota_mark_app_valid_cancel_rollback`. If the app crashes / hangs
   inside the window, the bootloader rolls us back on the next reset.

Runtime knobs (NVS namespace `"ota"`):

| key | type | default | meaning |
|---|---|---|---|
| `auto` | u8 | `1` (follows `CONFIG_TAB5_OTA_ENABLED`) | auto-poll gate |
| `base_url` | str | (empty → Kconfig default) | polling base URL |
| `pinned_tag` | str | (empty) | if set, only that tag installs |
| `rollback_cnt` | u32 | 0 | number of bootloader rollbacks observed |

Full design + threat model: [docs/OTA_UPDATE_DESIGN.md](docs/OTA_UPDATE_DESIGN.md).

## Tailscale NAT traversal

See [docs/TAILSCALE_PORTING_NOTES.md](docs/TAILSCALE_PORTING_NOTES.md) for the
full protocol notes, including:

- DISCO wire format (Ping / Pong / CallMeMaybe), NaCl box construction,
  sender disco-pub embedding
- WireGuard udp_pcb sharing for DISCO ↔ WG socket reuse (so NAT mapping is unified)
- DERP-initial endpoint + multi-probe DISCO + first-Pong-wins promotion
- HTTPS public-IP probe instead of UDP STUN (the latter fails in MAP-E /
  IPv4-over-IPv6 environments common in Japan)
- Outbound CallMeMaybe with rate-limit + verified-skip

These notes are intended for cross-porting to other ESP-IDF projects that
include the same `wireguard` and `tailscale` components.

## License

Source code originally written for this repository is licensed under the
**Boost Software License 1.0** — see [LICENSE](LICENSE).

The following third-party components are **vendored or referenced as
submodules** and carry their own licenses:

- `components/M5GFX/`, `components/M5Unified/` — git submodules. Each is under
  the upstream M5Stack license (MIT). Both carry local patches for ESP-IDF 6.0;
  see [M5_IDF6_PATCHES.md](M5_IDF6_PATCHES.md).
- `components/libssh2/libssh2/` — git submodule, BSD-3-Clause.
- `components/wireguard/`, `components/tailscale/`,
  `components/usb_host_ftdi_sio/` — vendored from
  [ciniml/serial_wifi_logger](https://github.com/ciniml/serial_wifi_logger),
  BSD-3-Clause (per upstream SPDX headers).
- ESP-IDF managed components (`espressif/esp_hosted`, `espressif/esp_wifi_remote`,
  `espressif/usb`, etc.) — pulled from Espressif's registry, with their own
  individual licenses.

Refer to each component's own license header / `LICENSE` file for terms.

## Acknowledgements

- **M5Stack** — Tab5 hardware and the M5GFX / M5Unified libraries
- **Tailscale / WireGuard** — the protocols this project implements a subset of
- [ciniml/serial_wifi_logger](https://github.com/ciniml/serial_wifi_logger) —
  the original ESP32 Tailscale / WireGuard implementation that this project
  extends substantially
- **Espressif** — ESP-IDF, esp_hosted, the C6 / P4 silicon
