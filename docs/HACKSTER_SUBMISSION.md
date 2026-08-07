# CONFIRM BEFORE PUBLISHING

Short list of things I could **not** verify from the repository. Fix or delete each
line in the body before you paste this into Hackster.

1. **"libssh2 blocks the move to ESP-IDF 6.0 stable because mbedTLS 4 made crypto
   headers private."** Not found anywhere in the repo. What the repo *does* say
   (`components/libssh2/CMakeLists.txt`, lines 1 and 25-36) is that libssh2 builds
   against **mbedTLS 3.6 / IDF 6.0**, and that IDF 6.0.2's TF-PSA-Crypto restructure
   moved legacy headers so the include paths had to be listed explicitly. CI
   (`.github/workflows/release.yml`) pins an ESP-IDF **master snapshot commit**
   (`6534932340b7140dfec19a9f24248fa9568c61d0`) rather than a released tag. I wrote the
   limitation as "IDF version pinning", which is what the repo supports. Rewrite it if
   the mbedTLS 4 story is real.
2. **"C6 auto-update is parked."** No such note in the repo. I left it out of the body.
   Add it back yourself if you want it stated.
3. **Test count.** `README.md` says 52 GoogleTest cases; the actual count in
   `host_test/tests/` today is **59** (`TEST`/`TEST_F` macros across 7 files). The body
   says 59. Consider also updating `README.md` before the judges look.
4. **Throughput numbers** (DERP ≈ 32 KB/s, direct WireGuard 200+ KB/s, LAN baseline
   ≈ 395 KB/s) come from `docs/TAILSCALE_PORTING_NOTES.md` §9 — your own bench
   measurements via the `ssh: perf rx=… B/s` meter. They are attributed as such in the
   body. Do not restate them as anything more precise.
5. **Product links.** I linked only `docs.m5stack.com`-style names I am *not* certain of
   by URL, so the hardware table names the products without guessing store URLs except
   for the Tab5 page. Verify the one link before publishing, or drop it.
6. **Video/photo placeholders** — the body has `[VIDEO]` / `[PHOTO n]` markers. Replace
   them with your actual uploads (Hackster inserts media inline; the markers are only
   there so you don't forget a slot).

---

# Portable Terminal — a hardware SSH terminal that reaches home from anywhere

A battery-powered, cable-free SSH/Telnet terminal built on the **M5Stack Tab5
(ESP32-P4)**. It renders a real 80×30 VT100 on the Tab5's 1280×720 MIPI-DSI panel,
and — the part that makes it more than a serial console — it speaks **Tailscale**.
Not "WireGuard with a hardcoded endpoint": an actual from-scratch Tailscale client
for ESP32, with the Noise control plane, DERP relay, and the **DISCO** NAT-traversal
protocol, so the device negotiates a *direct* UDP path to your machines. No jump
host to rent, no port forwarding, nothing to keep running at home except the box you
wanted to reach in the first place.

[VIDEO — provisioning + SSH over Tailscale, one take]

## The problem

Pocket SSH clients exist; they're phone apps. A dedicated hardware terminal boots into
a terminal, has a real keyboard, and doesn't ring. The hard part isn't the terminal
emulator — it's the network.

Your lab machine is behind NAT. Your phone tethering is behind CGNAT. And in Japan, a
lot of home fibre is **MAP-E** (IPv4-over-IPv6 — v6プラス, transix, OCN
バーチャルコネクト), where you share one public IPv4 address with fifteen other
households and get a small slice of the port range. Every usual answer is bad: port
forwarding doesn't exist under MAP-E; a cloud jump host is a monthly bill and a second
thing to secure; a plain relay works but crawls.

Tailscale solves exactly this on laptops. Getting it onto a microcontroller was the
project.

## What it does

- **80×30 VT100/ANSI terminal** — UTF-8, East-Asian wide characters, alternate screen
  buffer (`?1049`/`?1047`/`?47`), DSR responses, and enough CSI coverage to run `vim`,
  `htop`, and other full-screen TUIs.
- **SSH** (libssh2 1.11.1 + mbedTLS, password or embedded PEM key auth, **TOFU**
  host-key pinning in NVS) and **Telnet** (RFC 854 + NAWS window-size reporting).
- **USB-serial** (FTDI) and UART input — it's a serial console when there's no network.
- **Tailscale / WireGuard** with DERP relay fallback and DISCO direct-path negotiation.
- **Auto-reconnect** supervisor: backoff 1 s → 30 s, TCP keepalive (30 s / 10 s / 3
  probes) plus SSH keepalive driven once a second, so a dead link surfaces in about a
  minute instead of hanging forever.
- **Provisioning from a phone** — on-LCD QR codes, captive portal, HTTP settings page.
  A factory-fresh unit never touches a PC.
- **OTA updates** from GitHub Pages with bootloader rollback protection, plus a browser
  **Web Flasher** for the initial install.
- **Screen lock** — idle backlight-off, PIN pad on wake (SHA-256 hash in NVS,
  escalating lockout after 5 wrong tries).
- Touch, soft keyboard, clip-on Tab5 QWERTY, and USB-A HID keyboards — all four feed
  the same input sink.

## How the M5Stack Tab5 is used

The Tab5 is the whole machine; there are no other parts. Nearly every subsystem on the
board is doing real work:

| Tab5 capability | How the project uses it |
|---|---|
| **ESP32-P4** dual-core RISC-V | Terminal parser, SSH/TLS crypto, WireGuard data plane, and the DISCO/NaCl box work all run concurrently as FreeRTOS tasks. X25519 is milliseconds on this class of part, so DISCO keeps a per-peer shared-key cache. |
| **32 MB PSRAM** | A 1280×720×16bpp framebuffer is ~1.76 MB — PSRAM is not optional here. Configured hex-mode @ 200 MHz (`sdkconfig.defaults.esp32p4`). |
| **1280×720 MIPI-DSI LCD** | 80×30 grid at 12×24 px = 960×720, centred; the right margin at x=1120 (160 px) is a live status panel — battery, charge state, Wi-Fi IP, session state, uptime. When the soft keyboard is up the grid shrinks to 80×14 and the new pty size is pushed to the remote over SSH WINCH / Telnet NAWS. |
| **GT911 capacitive touch** | 60 Hz polling task emitting Down/Move/Up edges; drives the soft keyboard, the ☰ menu, the profile editor, and the PIN pad. |
| **On-board ESP32-C6 over SDIO** | The Tab5's Wi-Fi radio is a separate chip. We use `esp_hosted` + `esp_wifi_remote` so standard `esp_wifi_*` calls work transparently. PI4IOE2 (I²C 0x44, bit 0) is driven high at runtime to power the C6. |
| **Speaker (I²S codec)** | BEL (0x07) → `M5.Speaker.tone(880, 60)` via the `IDisplay::bell()` HAL hook. |
| **Battery gauge / charge status** | `M5.Power.getBatteryLevel()` + `isCharging()` in the status panel. |
| **ExtPort1 I²C** | The clip-on Tab5 Keyboard (addr 0x6D, SDA=GPIO0, SCL=GPIO1, INT=GPIO50). Standalone driver on `esp_driver_i2c` + a GPIO ISR — M5Unified already owns I²C 0 and 1, so this sits on the P4's LP_I2C controller. Firmware is put in HID mode so it reports standard USB HID Keyboard Page scancodes. |
| **USB-A host port** | HID keyboards and FTDI USB-serial adapters, via `usb_host_ftdi_sio` and a minimal HID host driver. |

Both keyboards share one translator (`main/hid_translate.cpp`) because both speak HID
usage page 0x07 — the clip-on unit and a random USB keyboard are literally the same
code path.

## Architecture

```
 touch ─┐
 soft-kbd ─┤
 Tab5 kbd ─┼─→ ByteSink ─→ IConnection ─→ SSH (libssh2) ──┐
 USB HID ─┤                             ─→ Telnet ────────┤
 UART/JTAG ┘                            ─→ USB-serial ────┤
                                                          │
        term_core (VT100 parser + screen model)  ←────────┘
                     │  IDisplay  (single HAL boundary)
      ┌──────────────┴──────────────┐
  display_m5gfx (Tab5 LCD)     fake_display (host tests)

 network:  lwIP ─→ WireGuard netif ─→ { direct UDP :41641 | DERP relay }
                        ↑ DISCO Ping/Pong/CallMeMaybe shares the same udp_pcb
```

`components/term_core` is hardware-independent C++23 and knows nothing about M5GFX.
It builds twice: once inside ESP-IDF, once as a plain CMake project against the host
compiler, where `host_test/fakes/fake_display.hpp` records draw calls into a 2D
buffer. That single `IDisplay` interface is why the terminal emulator can be
unit-tested at all.

## The interesting part: Tailscale on a microcontroller

Getting a relayed Tailscale connection up is the easy half — control plane over
Noise/HTTP2 to `login.tailscale.com`, netmap parsing, and a DERP websocket. Traffic
works, and it crawls. Getting a **direct** path is where the work is.

**DISCO.** Tailscale's discovery protocol rides the same UDP port as WireGuard:
`magic[6] "TS💬" || senderDiscoPub[32] || nonce[24] || NaCl box`. The inner payload is
Ping (`TxID || NodeKey`), Pong (`TxID || SrcIP || SrcPort`), or CallMeMaybe (a list of
18-byte AddrPort records). Four details had to be exactly right, and each one fails
silently when it isn't: the box is **NaCl** (XSalsa20-Poly1305), not XChaCha20; the
sender's disco public key must be in the outer frame or the peer can't tell who
pinged; DISCO must share WireGuard's `udp_pcb` so the NAT mapping is *one* mapping
rather than two; and you must answer inbound Pings with a Pong echoing the caller's
observed source address, or the peer never promotes you out of the relay. The dispatch
happens at the top of `wireguardif_network_rx` on the DISCO magic — which means
DERP-relayed DISCO frames take the identical path for free.

**The MAP-E problem.** The normal way to learn your own WAN endpoint is UDP STUN.
It was implemented, and on the author's dual-WAN MAP-E line it produced *zero*
responses across five probes at 30 s intervals — a debug hook at the very top of the
lwIP receive path confirmed no STUN-magic packet ever arrived, while HTTPS on the
same link was perfectly healthy. So the STUN module was deleted and replaced with
`tailscale_pubip.c`: an HTTPS GET to `api.ipify.org` (falling back to
`checkip.amazonaws.com`) on its own 6 KB task, 5-minute refresh, deduplicated by
(ip, port), assuming port preservation for `CONFIG_TAILSCALE_LISTEN_PORT`. The result
is published in `MapRequest.Endpoints`, with a dirty flag that forces a fresh
MapRequest so peers learn the endpoint without waiting for their own netmap refresh.

**Closing the loop faster.** Waiting for the peer to re-fetch its netmap is minutes.
So the device also *sends* CallMeMaybe over DERP — "I'm at these addresses, ping me
now" — gated three ways: skip if already verified direct, rate-limit to one per peer
per 30 s, skip if no endpoints are known yet. Meanwhile the WireGuard endpoint is
always **initialised to the DERP pseudo-address** (127.3.3.40:region) so connectivity
exists from second zero, DISCO Pings fan out to all candidates (raised from 4 to 8,
with public-WAN candidates score-boosted so they aren't crowded out by a peer's Docker
bridge IPs), and the first Pong wins and promotes the peer to direct. A heartbeat plus
peer-initiated Ping handling keeps a verified direct path from silently decaying.

The measured effect, from the on-device `ssh: perf rx=` meter and recorded in
`docs/TAILSCALE_PORTING_NOTES.md`: DERP relay ≈ **32 KB/s**, direct WireGuard
**200+ KB/s**, with a same-LAN baseline of ≈ 395 KB/s. Over LTE tethering from
outside, that's the difference between unusable and comfortable.

Known limits of the implementation are documented rather than hidden: IPv6 peers are
not handled (non-v4-mapped CallMeMaybe addresses are dropped), there's no automatic
NAT-type classification, and on a dual-WAN line the HTTPS probe can report the PPPoE
address on one boot and the MAP-E address on the next.

## Onboarding: no cable, no toolchain

[PHOTO — QR overlay on the LCD, phone camera framing it]

A factory-fresh unit has no credentials, so it can't join anything — the classic
chicken-and-egg. The boot path handles it: with no stored STA credentials it powers
the C6, initialises Wi-Fi *without* joining, and brings up a WPA2 softAP directly.

1. Power on. The LCD shows a full-screen overlay with a **Wi-Fi QR**
   (`WIFI:T:WPA;…` — the stock phone camera joins the AP straight from it), a **URL QR**
   for `http://192.168.4.1/`, and the credentials in plain text.
2. The AP is `Tab5-XXXXXX` with a **per-device 10-character password**, generated on
   first boot and persisted in NVS (ambiguous `0/o/1/l` excluded). It's also printed in
   the boot log, and `erase-flash` regenerates it.
3. A captive portal — a UDP/53 responder answering every query with 192.168.4.1, plus
   302s on `/generate_204`, `/hotspot-detect.html`, `/connecttest.txt` and unknown
   paths — pops the settings page automatically on most phones.
4. Fill in Wi-Fi, the connection profile (SSH/Telnet host, port, user, password), and
   the Tailscale auth key. Secrets are write-only: `/api/info` reports `*_set` booleans
   and never the values.
5. Reboot from the page. If Tailscale needs interactive login, a **third QR with the
   login URL** appears on the LCD — scan, approve in the browser, done.

The same overlay reappears automatically if a Wi-Fi join later fails, which is exactly
when you need it, and can be summoned from the ☰ menu on demand.

## Hardware used

| Item | Required? | Notes |
|---|---|---|
| **M5Stack Tab5** (ESP32-P4 + on-board ESP32-C6) | **Required** — this is the M5Stack controller | 1280×720 MIPI-DSI, GT911 touch, 32 MB PSRAM, 16 MB flash, battery, speaker, USB-A host, ExtPort1. <https://shop.m5stack.com/products/m5stack-tab5-esp32p4-iot-development-kit> |
| **M5Stack Tab5 Keyboard** (clip-on QWERTY) | Optional | I²C 0x6D on ExtPort1. Without it, the on-screen soft keyboard covers everything. |
| **Any USB HID keyboard** | Optional | Plugs into the Tab5's USB-A host port. |
| **FTDI USB-serial adapter** | Optional | Only for using the Tab5 as a serial console. |

No breadboard, no soldering, no extra modules. Everything else is firmware.

## Build and install

**Repository: <https://github.com/ciniml/portable_terminal>**

**Path A — Web Flasher (no toolchain).** Open
<https://ciniml.github.io/portable_terminal/> in Chrome or Edge, plug the Tab5 in over
USB, pick a release, click Flash. It's esptool-js over WebSerial; the page is built by
CI from tagged releases. Artifacts are also on
[GitHub Releases](https://github.com/ciniml/portable_terminal/releases).

**Path B — from source.**

```bash
git clone --recursive https://github.com/ciniml/portable_terminal.git
cd portable_terminal
source ~/esp-idf/6.0/export.sh
make set-target        # idf.py set-target esp32p4, one time
make build
make flash monitor
```

**One extra step, and it matters:** the Tab5 ships with M5Stack's C6 Wi-Fi firmware
(V1.4.1, `esp_hosted` 1.4.x), which is incompatible with the 2.x host stack this
project uses. `c6_updater/` is a self-contained IDF 5.4 project that speaks the
*factory-compatible* protocol over SDIO and pushes the new slave firmware — no cover
removal:

```bash
./c6_updater/updater.sh /dev/ttyACM0   # wait for "Slave update completed"
```

An ESP-Prog UART fallback is documented in `slave_c6_fw/README.md` if that fails. Full
details and all Kconfig options are in the repo
[README](https://github.com/ciniml/portable_terminal#readme).

## Testing and quality

The VT100 core is covered by **59 GoogleTest cases** across 7 files (parser, screen
model, wide characters, CSI extensions, alternate screen, DSR, and golden end-to-end
sequences), run on the host with `make test-host` — no hardware in the loop. That's how
a terminal emulator gets to be boring in the good way.

Releases are tag-driven: pushing `vX.Y.Z` builds in an ESP-IDF container against a
pinned toolchain commit, attaches the artifacts to a GitHub Release, and a second
workflow stages every release into GitHub Pages with a `versions.json` the Web Flasher
reads. The device then polls `latest.json` every 6 h (±15 min jitter, seeded from the
STA MAC), SemVer-compares against its own `esp_app_desc`, downloads in 256 KB partial-
HTTP chunks so a Wi-Fi flap doesn't restart the transfer, and **defers the reboot until
no session is active** (up to 30 min). After the new image boots it waits 120 s of
healthy operation before calling `esp_ota_mark_app_valid_cancel_rollback` — crash
inside that window and the bootloader rolls back on the next reset. Layout is 16 MB
flash, two 3 MB OTA slots, current app ≈ 2.4 MB.

## Known limitations / what's next

- **IPv6 peers aren't supported** in DISCO — CallMeMaybe records that aren't
  IPv4-mapped are dropped. Native IPv6 would sidestep NAT traversal entirely, so this
  is the highest-value next item.
- **No NAT-type classification.** The official client probes STUN repeatedly to detect
  symmetric NAT; this one just tries direct and falls back to DERP.
- **Public-IP probe is route-dependent** on dual-WAN lines and can report the wrong
  side; peer-side WireGuard roaming usually recovers it.
- **One session at a time.** `IConnection` and the profile store are already
  multi-session-shaped; the UI isn't yet.
- **Toolchain pinning.** The build tracks an ESP-IDF 6.0 development snapshot rather
  than a released tag; the vendored M5GFX/M5Unified forks carry IDF 6.0 patches
  (`M5_IDF6_PATCHES.md`).
- **The screen lock is casual privacy, not security** — flash isn't encrypted, and
  anyone with the softAP password can reset the PIN. Documented as such on purpose.

## Repository and license

<https://github.com/ciniml/portable_terminal>

Code written for this project is **Boost Software License 1.0**. Vendored and submoduled
components keep their own licenses: M5GFX / M5Unified (MIT, patched forks), libssh2
(BSD-3-Clause), and the `wireguard` / `tailscale` / `usb_host_ftdi_sio` components
originating from [ciniml/serial_wifi_logger](https://github.com/ciniml/serial_wifi_logger)
(BSD-3-Clause), which this project extends substantially — the DISCO rewrite, the
HTTPS public-IP probe, CallMeMaybe in both directions, and the direct-path liveness
work are new here, and `docs/TAILSCALE_PORTING_NOTES.md` exists specifically so they
can be back-ported to other ESP-IDF projects.

---
---

# APPENDIX A — Photo / video shot list

You have one evening. Shoot in this order and stop when you run out of time; anything
below the line still leaves a complete submission.

### MUST HAVE

**1. The hero video — one continuous take, 90-150 s, no cuts. (~40 min incl. retakes)**

This single clip answers three of the four judging criteria at once, and "unedited
single take" is itself the credibility argument.

Sequence: hold up a Tab5 that has just been `erase-flash`ed → power on → QR overlay
appears → phone camera scans the Wi-Fi QR, joins → captive portal pops → type Wi-Fi
SSID/password and the Tailscale auth key → reboot → Tailscale login QR appears on the
LCD → scan, approve in the phone browser → device comes up → SSH session to a home
machine opens → **type something that proves it's real and remote**: `hostname`,
`ip a`, then launch `htop` or `vim` so a full-screen TUI redraws.

Shooting notes: phone on anything that holds it still, device flat, room lights on,
kill autofocus hunting by locking focus (tap-and-hold). Narrate briefly or add
captions later — Hackster judges often watch muted, so if you can only do one, add
on-screen text at the three transitions. If a step fails on camera, *keep the take* if
it recovers; recovery on camera is more persuasive than a clean cut.

**Why it scores:** Functionality & Execution (it visibly works end-to-end),
Documentation & Presentation (reproducibility — a viewer sees the whole path),
Impact & Usefulness (the zero-PC provisioning story lands instantly).

**2. Hero still — the Tab5 running a real SSH session, held in one hand. (~5 min)**

This is your Hackster cover image. Landscape, terminal text legible, status panel
visible on the right. Shoot several exposures; LCDs blow out easily — tap to meter on
the screen, not the room. A dark-ish desk background makes the panel pop.

**Why it scores:** it's the thumbnail. Everything else is downstream of someone
clicking.

**3. The Tailscale proof shot — split evidence. (~10 min)**

Either (a) a photo of the Tab5 next to a laptop showing `tailscale status` with the
`tab5` node listed as **direct** (not `relay`), or (b) a screen capture of the serial
monitor showing the log lines `ts_pubip: discovered …`, `ts_disco: Pong recv from … →
peer N direct`, and a `ssh: perf rx=…` figure. (b) is stronger for technical judges;
(a) is stronger for skimmers. Do (a) if you only do one.

**Why it scores:** Creativity & Originality. This is the only shot that proves the
claim the whole project rests on. Without it, "implements Tailscale" is an assertion.

**4. Hardware-in-context shot with the clip-on keyboard attached. (~5 min)**

Shows the M5Stack hardware integration physically — Tab5 + Tab5 Keyboard, being typed
on. Satisfies "list of hardware used" visually.

### NICE TO HAVE (do these only if time remains)

**5. QR overlay close-up** — a clean, legible still of the LCD showing both QR codes.
Useful inline in the onboarding section even though the video covers it.

**6. Soft keyboard + terminal reflow** — a 10 s clip toggling the on-screen keyboard,
showing the grid resize from 30 rows to 14 and the remote reflowing. Cheap to shoot,
demonstrates the NAWS/WINCH plumbing that most projects skip.

**7. Screen lock** — 10 s: idle → backlight off → touch → PIN pad → unlock. Shows UI/UX
polish.

**8. Web Flasher screen recording** — 20 s of the browser flashing a Tab5. Strong for
"reproducibility" but it's a screen recording, so it's the easiest thing to add
tomorrow if the deadline slips into the buffer.

**9. `make test-host` terminal output** — a screenshot of 59/59 passing. One command,
five seconds, and it's the only evidence of the testing story.

### Skip
Beauty shots, unboxing, slow-motion anything, a title card. They cost time and score
nothing.

---

# APPENDIX B — Submission checklist

**Deadline: 2026-08-07 23:59 PST = 2026-08-08 16:59 JST.** Work backwards from that.

1. **Log in to Hackster.io** (or create an account). Confirm the email is one you can
   receive on — contest contact goes there.
2. **Create a new project.** Hackster: *Projects → Add a new project → From scratch*.
   Title suggestion: *Portable Terminal — a hardware SSH terminal that reaches home
   over Tailscale*.
3. **Set the project to "Work in progress" while you edit**, so a half-built page isn't
   indexed. You will flip it to published at step 10.
4. **Paste the body** (everything from the `# Portable Terminal` heading down to the
   license section — **not** the CONFIRM block, **not** these appendices) into the
   project story editor. Hackster's editor accepts Markdown paste for headings, lists,
   tables, and fenced code; check that the ASCII architecture diagram survived — if it
   reflows, wrap it in a code block manually.
5. **Upload media.** Cover image = shot 2. Then replace each `[VIDEO]` / `[PHOTO n]`
   marker with the real upload. For the hero video, upload to YouTube (unlisted is
   fine) and embed the link — Hackster embeds YouTube cleanly and it's more reliable
   than a direct upload the night of a deadline.
6. **Fill the structured fields**, which judges filter on and which are separate from
   the story text:
   - *Components/Hardware*: add **M5Stack Tab5** (search the component database; add as
     custom if absent), plus the Tab5 Keyboard and a generic USB keyboard.
   - *Software apps and online services*: ESP-IDF, Tailscale, GitHub Actions.
   - *Tags/Topics*: `ESP32`, `M5Stack`, `Tailscale`, `WireGuard`, `SSH`, `VPN`,
     `terminal`, `NAT traversal`.
7. **Add the code repository link** — Hackster has a dedicated *Code* section; add
   `https://github.com/ciniml/portable_terminal` as a GitHub repo link there **as well
   as** in the body. Judges look for the structured field.
8. **Add the Web Flasher link** (`https://ciniml.github.io/portable_terminal/`) in the
   story — it's the "anyone can reproduce this" proof.
9. **Proofread the CONFIRM list items** and make sure none of them survived into the
   published text.
10. **Publish.** Set visibility to **public / published**. The contest requires the
    project to be *first published on Hackster.io in 2026* — confirm the published date
    shows on the page after publishing, and take a screenshot of it as your own record.
11. **Copy the live project URL.**
12. **Submit the Google Form: <https://forms.gle/9GozJXJ3XFDAqDjW7>** with that URL.
    This is the step that actually enters you; publishing alone does not.
13. **Save the form confirmation** (screenshot or the confirmation email).

Time-boxing suggestion if you're tight: steps 1-4 and 7 first (30 min — the page exists
and has the repo link), then shoot shots 1-4 (60-90 min), then steps 5-6, then publish
and submit with at least an hour of margin. **Submit the form before polishing the
page** — you can keep editing a published Hackster project after submission, but you
cannot submit after the deadline.
