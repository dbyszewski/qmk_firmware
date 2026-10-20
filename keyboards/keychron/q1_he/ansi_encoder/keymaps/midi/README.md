# Keychron Q1 HE (ANSI knob): Hall effect MIDI keymap

Keychron firmware (branch `2025q3`, 1.3.1) with a USB MIDI mode that uses the magnetic switches:
velocity from strike speed, aftertouch from key depth, pressure sliders and drum pads.

## MIDI mode (Fn+Esc toggles)

| Keys | MIDI |
|---|---|
| Z row + A row | notes from C3 (white keys on Z–/, black on S D G H J L ;) |
| Q row + number row | one octave up (black on 2 3 5 6 7 9 0 =) |
| ← / → | octave down / up |
| ↑ / ↓ | transpose ±1 semitone |
| Fn + ↑ / ↓ | velocity sensitivity (5 levels, shown on F1–F5 while Fn is held) |
| Space | sustain (CC64) |
| F1–F4 | pressure sliders CC74, CC71, CC91, CC93 (hold still ~30 ms to set, value stays after release) |
| F5–F12 | drum pads on channel 10: GM 36 38 42 46 45 50 49 51 |
| Knob | mod wheel (CC1); press = all notes off |

Velocity is the time from 0.3 mm to 1.5 mm (log of 2–200 ms, gamma curve per sensitivity level).
Polyphonic and channel aftertouch start after a note is held 150 ms, from 2.8 mm to the bottom (4.08 mm).
Keys outside the map type normally, so Fn+Esc always works.

Other changes against the stock `keychron` keymap:

- Fn layer: F13–F24 on Y I O P H J K L X C V M (host shortcuts), `[` `]` and base-layer Home send nothing
  (the host reads their depth for push-to-talk and pressure volume), Fn+Del = bootloader.
- XInput is disabled: the STM32F401 has three endpoints and MIDI needs one (the HID gamepad stays).
- `analog_matrix_midi_hook()` in `keyboards/keychron/common/analog_matrix/analog_matrix.c` lets the keymap
  take a key's travel before the normal trigger actions.

## Build and flash

```
make keychron/q1_he/ansi_encoder:midi ALLOW_WARNINGS=yes   # GCC 16 warns in lkbt51.c
keyboards/keychron/q1_he/ansi_encoder/keymaps/midi/flash.sh # Fn+Del (or hold Esc while plugging in)
```

`flash.sh` fills flash sector 1 (0x08004000–0x08007FFF, the emulated EEPROM) with 0xFF before flashing:
`objcopy` pads that gap with zeros, which would leave garbage in the settings area.
The Hall effect calibration lives in the external I2C EEPROM and survives flashing.
