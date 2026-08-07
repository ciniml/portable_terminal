#!/usr/bin/with-contenv bash
# Install the terminal-showcase tools on every container start.
#
# `apk add` writes to the container filesystem, which is thrown away when the
# container is recreated — only /config is a volume. linuxserver images run
# everything in /config/custom-cont-init.d/ during init, so putting the
# install here makes it survive `docker compose down && up`.
set -e

PKGS="htop vim mc ncdu figlet cmatrix sl bat tmux ncurses"

# Skip the network round-trip when everything is already present (fast restart).
missing=""
for p in $PKGS; do
    apk info -e "$p" >/dev/null 2>&1 || missing="$missing $p"
done
[ -z "$missing" ] && { echo "[demo-tools] all present"; exit 0; }

echo "[demo-tools] installing:$missing"
apk add --no-cache $missing
