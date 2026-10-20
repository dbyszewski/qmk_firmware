VIA_ENABLE = yes
MIDI_ENABLE = yes
SRC += midi_hall.c

# STM32F401 has 3 endpoints: XInput, raw HID and the shared one are already taken.
# Drop XInput (the plain HID gamepad in the shared endpoint stays) to make room for MIDI.
OPT_DEFS := $(filter-out -DXINPUT_ENABLE,$(OPT_DEFS))
