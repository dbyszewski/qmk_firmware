#!/usr/bin/env bash
# Flash the midi keymap build with dfu-util. Enter the bootloader with Fn+Del
# (or hold Esc while plugging in); the script waits for it.
set -euo pipefail

cd "$(dirname "$0")/../../../../../.."
bin=keychron_q1_he_ansi_encoder_midi.bin
[ -f "$bin" ] || { echo "build first: make keychron/q1_he/ansi_encoder:midi ALLOW_WARNINGS=yes" >&2; exit 1; }

img=$(mktemp --suffix=.bin)
trap 'rm -f "$img"' EXIT
# Sector 1 holds the emulated EEPROM; objcopy pads it with zeros, flash it erased instead.
python3 -c "
import sys
b = bytearray(open(sys.argv[1], 'rb').read())
b[0x4000:0x8000] = b'\xff' * 0x4000
open(sys.argv[2], 'wb').write(b)
" "$bin" "$img"

echo "waiting for the DFU bootloader (Fn+Del)..."
until grep -lqs df11 /sys/bus/usb/devices/*/idProduct; do sleep 1; done
sleep 1
dfu-util -a 0 -s 0x08000000:leave -D "$img"
