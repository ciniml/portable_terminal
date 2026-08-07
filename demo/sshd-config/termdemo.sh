#!/bin/sh
# Guided VT100 feature tour for the portable_terminal client.
#
# Each step exercises a different part of components/term_core: SGR colour,
# cursor addressing, scroll regions, the alternate screen, and East Asian
# width handling. Press Enter to advance so it can be narrated on camera.
#
#   ssh demo@tab5-demo-sshd -p 2222 -t /config/termdemo.sh

esc() { printf '\033%s' "$1"; }
pause() { printf '\n\033[2m-- Enter --\033[0m'; read -r _; }
title() { printf '\033[2J\033[H\033[1;36m== %s ==\033[0m\n\n' "$1"; }

title "1. SGR colour and attributes"
printf '  \033[31mred\033[0m \033[32mgreen\033[0m \033[33myellow\033[0m'
printf ' \033[34mblue\033[0m \033[35mmagenta\033[0m \033[36mcyan\033[0m\n'
printf '  \033[1mbold\033[0m \033[2mdim\033[0m \033[4munderline\033[0m'
printf ' \033[7mreverse\033[0m\n\n  256-colour ramp:\n  '
i=16
while [ $i -lt 232 ]; do
    printf '\033[48;5;%dm \033[0m' $i
    i=$((i + 1))
    [ $(((i - 16) % 36)) -eq 0 ] && printf '\n  '
done
printf '\n'
pause

title "2. East Asian width (fullwidth cells)"
printf '  ASCII    : |ABCDEFGHIJ|\n'
printf '  Japanese : |あいうえお|      <- 5 chars, 10 columns\n'
printf '  Mixed    : |A あ B い C|\n\n'
printf '  端末エミュレータは全角文字を 2 セル幅で扱います。\n'
printf '  カーソル位置とダメージ矩形も 2 セル単位で計算されます。\n'
pause

title "3. Cursor addressing and box drawing"
printf '\033[6;10H+----------------------+'
printf '\033[7;10H|  absolute cursor     |'
printf '\033[8;10H|  positioning (CUP)   |'
printf '\033[9;10H+----------------------+'
printf '\033[12;1H  Drawn with CSI row;colH, not by printing newlines.\n'
pause

title "4. Alternate screen"
printf '  The next apps switch to the alt screen (DECSET ?1049),\n'
printf '  draw over it, then restore this text on exit.\n'
printf '\n  Try:  htop     (scroll regions, full redraw)\n'
printf '        mc       (box drawing, panels)\n'
printf '        vim      (modal editing, status line)\n'
printf '        ncdu /   (progress, tree view)\n'
pause

title "5. Eye candy"
printf '  figlet Tab5 | cmatrix -u9 | sl\n\n'
figlet -w 78 Tab5 2>/dev/null
printf '\n  Ctrl-C stops cmatrix.\n'
pause

printf '\033[2J\033[H\033[1;32mTour complete.\033[0m  Shell follows.\n\n'
exec "${SHELL:-/bin/sh}" -l
