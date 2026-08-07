# Tailscale SSH demo server

A throwaway SSH server, reachable over Tailscale, for demoing the Tab5 client.
Runs as two Docker containers on this host and shows up in your tailnet as a
single node named **`tab5-demo-sshd`** (its own `100.x` address).

```
your tailnet (cinimlm@)
  ├─ this host            (already joined)
  ├─ tab5-demo-sshd  <--- this compose project: tailscaled + sshd
  └─ tab5                 (the device, joins via pre-auth key)
        └─ libssh2 SSH to  tab5-demo-sshd:2222  over the tailnet
```

Why a container instead of the host's own sshd: it's a disposable, isolated
target with throwaway credentials, and it doesn't touch this machine's real
SSH config. The host already runs Tailscale, but the container is a separate
network namespace, so the two tailscaled instances don't conflict.

## Prerequisites (already verified on this host)

- Docker + compose
- `/dev/net/tun` present (so the container gets a real `tailscale0`)

## 1. Get a Tailscale pre-auth key

Admin console -> https://login.tailscale.com/admin/settings/keys -> Generate.
- **Reusable: ON** — survives re-creating the container / re-flashing the Tab5.
- **Pre-approved: ON** — no manual node approval step.

The same key can be reused for the Tab5 firmware config.

## 2. Start the server

```bash
cd demo
cp .env.example .env          # paste your tskey-auth-... (and tweak creds if you like)
docker compose up -d
docker compose logs -f ts     # wait for "Success." / 100.x assignment
docker compose exec ts tailscale ip -4   # <- note this IP for the Tab5 config
```

Confirm `tab5-demo-sshd` shows **online** in the admin console.

Sanity-check from any other tailnet machine:

```bash
ssh demo@<100.x>  -p 2222      # password: demo
```

## 3. Configure the Tab5

Copy the lines from [`tab5.sdkconfig.defaults.local.example`](tab5.sdkconfig.defaults.local.example)
into `<repo root>/sdkconfig.defaults.local`, set `CONFIG_TAB5_SSH_HOST` to the
IP from step 2, then from the repo root:

```bash
source ~/esp-idf/6.0/export.sh
rm -f sdkconfig && make build && make flash monitor
```

## 4. Exercise the terminal

The container carries a set of tools chosen to drive different parts of the
VT100 core, plus a guided tour script:

```bash
ssh demo@<ip> -p 2222 -t /config/termdemo.sh
```

The tour walks through SGR colour and attributes, a 256-colour ramp, East
Asian width (fullwidth cells count as two columns), absolute cursor
addressing, the alternate screen, and some eye candy — pausing at each step
so it can be narrated on camera. It drops into a normal shell at the end.

Individually, the interesting ones:

| Command | What it exercises |
|---------|-------------------|
| `htop` | alt screen, scroll regions, continuous full redraw |
| `mc` | box drawing, panels, function-key bar |
| `vim` | modal editing, status line, cursor shapes |
| `ncdu /` | progress rendering, tree view |
| `less /etc/services` | paging, backward scroll |
| `bat <file>` | 256-colour syntax highlighting |
| `figlet Tab5` | wide ASCII art (line wrapping) |
| `cmatrix -u9` | sustained full-screen throughput |
| `sl` | animation, right-to-left cursor motion |
| `tmux` | nested terminal, split panes |

Japanese text renders properly — the device uses `lgfxJapanGothic_24`, and
`components/term_core` tracks East Asian width so fullwidth glyphs occupy two
cells.

Tool installation happens in `sshd-config/custom-cont-init.d/10-demo-tools.sh`,
which the linuxserver image runs on every container start. `apk add` writes to
the container filesystem (thrown away on recreate), so the hook — living in the
`/config` volume — is what makes the tools survive `docker compose down && up`.

## Notes / gotchas

- **Routing**: Tab5 and the container are both behind NAT. If a direct path
  can't be punched, traffic falls back to a **DERP relay** automatically
  (`CONFIG_TAILSCALE_DERP_ONLY=n`). Do one live dry-run before the demo.
- **Host keys persist** in `./sshd-config`, so re-creating the container keeps
  the Tab5's TOFU fingerprint valid. If you ever wipe it and the key changes,
  reset the Tab5 trust store with `idf.py erase-flash`.
- **This host must be powered on and online** during the demo. Thanks to
  Tailscale the Tab5 can be on a different network.
- **ACLs**: default (allow-all) tailnets need nothing extra. If yours is
  locked down, allow `tab5` -> `tab5-demo-sshd:2222`.

## Teardown

```bash
docker compose down            # stop; keeps state in ts-state/ and sshd-config/
docker compose down -v ; rm -rf ts-state sshd-config   # full wipe
```

After a full wipe, remove the stale `tab5-demo-sshd` node from the admin console.

## Files

| File | Purpose |
|------|---------|
| `docker-compose.yml` | tailscaled + sshd sidecar definition |
| `.env.example` | template for `TS_AUTHKEY` + demo credentials (copy to `.env`) |
| `tab5.sdkconfig.defaults.local.example` | Tab5 firmware config snippet |
| `sshd-config/termdemo.sh` | guided VT100 feature tour (step 4) |
| `sshd-config/custom-cont-init.d/10-demo-tools.sh` | installs the demo tools on container start |
| `.gitignore` | keeps `.env` and runtime state out of git |

`sshd-config/` is a runtime volume and is otherwise gitignored; the two files
above are force-added so the demo is reproducible from a clean checkout.
