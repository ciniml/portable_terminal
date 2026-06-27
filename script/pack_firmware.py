#!/usr/bin/env python3
# Pack the per-partition binaries that `idf.py build` produced (referenced
# from <build-dir>/flash_args at their target addresses) into a single
# firmware.bin padded with 0xFF. This single image is convenient for
# WebSerial / M5Burner-style flash tools that want one file + one offset 0x0.

import argparse
import pathlib
import re


target_pattern = re.compile(r'^(0x[0-9a-fA-F]{1,8})\s+([\w\./-]+)')


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--build-dir', default='build',
                   help='IDF build directory (default: build).')
    p.add_argument('--out', default='firmware.bin',
                   help='Output filename (default: firmware.bin).')
    p.add_argument('--extra', action='append', default=[],
                   metavar='OFFSET:PATH',
                   help='Additional binaries to splice in at OFFSET '
                        '(hex, e.g. 0x710000). May be repeated. PATH is '
                        'resolved relative to the current working dir, '
                        'NOT the build dir, so absolute paths or '
                        '"build/c6_fw.bin"-style are both fine. Used to '
                        'bundle the c6_fw partition image (an ESP32-C6 '
                        'app that esptool refuses to keep in the main '
                        'flash_args without --force).')
    args = p.parse_args()

    build_dir = pathlib.Path(args.build_dir)
    flash_args = build_dir / 'flash_args'

    # Tuples: (offset, source_path). source_path is relative to
    # build_dir when it came from flash_args, absolute / cwd-relative
    # when it came from --extra. Resolve both consistently below.
    targets = []
    with open(flash_args) as f:
        for line in iter(f.readline, ''):
            m = target_pattern.match(line)
            if m:
                start_address = int(m.group(1), 16)
                path = m.group(2)
                targets.append((start_address, build_dir / path))

    for spec in args.extra:
        if ':' not in spec:
            raise SystemExit(f'--extra needs OFFSET:PATH, got {spec!r}')
        off_str, path = spec.split(':', 1)
        targets.append((int(off_str, 0), pathlib.Path(path)))

    targets.sort(key=lambda x: x[0])
    print(targets)

    with open(args.out, 'wb') as f:
        current_address = 0
        for start_address, bin_path in targets:
            if current_address < start_address:
                pad = b'\xff' * (start_address - current_address)
                f.write(pad)
                current_address += len(pad)
            with open(bin_path, 'rb') as g:
                data = g.read()
                f.write(data)
                current_address += len(data)


if __name__ == '__main__':
    main()
