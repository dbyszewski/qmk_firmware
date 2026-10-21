/* Hall effect MIDI mode for the Keychron Q1 HE.
 *
 * Velocity comes from the time a key takes to travel between two depths,
 * polyphonic and channel aftertouch from how far it is pressed past the note-on point.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Register MIDI input callbacks (notes from the host light the keys). Call once after USB MIDI setup. */
void midi_hall_init(void);

void midi_hall_toggle(void);

/* Call every scan: samples the pressure sliders (F1-F4). */
void midi_hall_task(void);
bool midi_hall_enabled(void);

/* Knob and helper keys, called from process_record_user while MIDI mode is on. */
void midi_hall_mod_wheel(int8_t delta);
void midi_hall_panic(void);

/* Paint the piano layout over the current RGB effect. */
void midi_hall_indicators(uint8_t led_min, uint8_t led_max);
