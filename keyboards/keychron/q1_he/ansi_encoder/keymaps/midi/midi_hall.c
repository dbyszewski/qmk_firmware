/* Hall effect MIDI mode for the Keychron Q1 HE. See midi_hall.h. */
#include QMK_KEYBOARD_H
#include <ch.h>
#include <math.h>
#include <string.h>
#include "qmk_midi.h"
#include "analog_matrix.h"
#include "analog_matrix_type.h"
#include "midi_hall.h"

/* Travel is reported in 1/60 mm (0.1 mm = 6), 240 = bottomed out. */
#define T_ARM       18   /* 0.3 mm: start timing the strike          */
#define T_NOTE_ON   90   /* 1.5 mm: note on, velocity from arm..here */
#define T_NOTE_OFF  54   /* 0.9 mm: note off (hysteresis)            */
#define T_AT_START  168  /* 2.8 mm: aftertouch starts                */
#define T_FULL      245  /* measured: every key reaches 4.08 mm      */

/* Velocity: log of the arm..note-on time mapped to 0..1, then shaped by the sensitivity curve. */
#define VEL_FAST_US 2000.0f   /* strike this fast or faster = 127 */
#define VEL_SLOW_US 200000.0f /* strike this slow or slower = 1 (slow, careful presses still sound) */
/* Fn + Up/Down in MIDI mode; < 1 = softer touch reaches loud notes, > 1 = harder. */
/* Tuned on the owner's playing: soft ~30, normal ~75, hard ~110+ at the default level. */
static const float sensitivity_gamma[] = {1.4f, 1.1f, 0.85f, 0.65f, 0.5f};
#define SENSITIVITY_LEVELS (sizeof(sensitivity_gamma) / sizeof(sensitivity_gamma[0]))
#define SENSITIVITY_DEFAULT 2

#define AT_MIN_STEP     2
#define AT_INTERVAL_US  10000
#define AT_DELAY_US     150000 /* a strike that bottoms out is not pressure: wait for a held note */

#define HALL_MIDI_CHANNEL 0
#define DRUM_CHANNEL      9    /* MIDI channel 10 */
#define BASE_NOTE    48   /* C3 on Z */
#define SUSTAIN_ON   60   /* 1.0 mm */
#define SUSTAIN_OFF  36   /* 0.6 mm */

/* Pressure sliders: the value follows the key while it is held still and stays after release. */
#define SLIDER_MIN     18     /* 0.3 mm = 0   */
#define SLIDER_MAX     234    /* 3.9 mm = 127 */
#define SLIDER_JITTER  3
#define SLIDER_HOLD_US 30000  /* a release is faster than this, so it does not move the value */

#define FN_ROW 5
#define FN_COL 10

/* Scales, Fn + 1..9 in MIDI mode: bit n = n semitones above the root. Out-of-scale keys are silent and dark. */
static const uint16_t scale_masks[] = {
    0xFFF,                                                        /* 1 chromatic (off) */
    1 << 0 | 1 << 2 | 1 << 4 | 1 << 5 | 1 << 7 | 1 << 9 | 1 << 11, /* 2 major           */
    1 << 0 | 1 << 2 | 1 << 3 | 1 << 5 | 1 << 7 | 1 << 8 | 1 << 10, /* 3 natural minor   */
    1 << 0 | 1 << 2 | 1 << 4 | 1 << 7 | 1 << 9,                   /* 4 major pentatonic */
    1 << 0 | 1 << 3 | 1 << 5 | 1 << 7 | 1 << 10,                  /* 5 minor pentatonic */
    1 << 0 | 1 << 2 | 1 << 3 | 1 << 5 | 1 << 7 | 1 << 9 | 1 << 10, /* 6 dorian          */
    1 << 0 | 1 << 2 | 1 << 4 | 1 << 5 | 1 << 7 | 1 << 9 | 1 << 10, /* 7 mixolydian      */
    1 << 0 | 1 << 2 | 1 << 3 | 1 << 5 | 1 << 7 | 1 << 8 | 1 << 11, /* 8 harmonic minor  */
    1 << 0 | 1 << 3 | 1 << 5 | 1 << 6 | 1 << 7 | 1 << 10,         /* 9 blues           */
};
#define SCALES (sizeof(scale_masks) / sizeof(scale_masks[0]))
#define SCALE_ROW 1   /* number row: 1..9 = columns 1..9 */

#define KNOB_COL 14
#define ESC_COL  0
#define DEL_COL  13

/* key_map codes: < PAD0 semitone offset from BASE_NOTE, then pads, sliders and controls. */
enum {
    PAD0 = 0x40,
    SLIDER0 = 0x50,
    OCT_DOWN = 0x70, OCT_UP, TRANS_DOWN, TRANS_UP, SUSTAIN,
    NONE = 0x7F,
};

static const uint8_t pad_notes[] = {36, 38, 42, 46, 45, 50, 49, 51}; /* GM: kick snare hats toms crash ride */
static const uint8_t slider_cc[] = {74, 71, 91, 93};                 /* brightness resonance reverb chorus */
#define PADS    (sizeof(pad_notes) / sizeof(pad_notes[0]))
#define SLIDERS (sizeof(slider_cc) / sizeof(slider_cc[0]))

/* Lower octave on Z/A rows, upper on Q/number rows, sliders F1-F4, pads F5-F12.
 * NONE keys keep working normally. */
static const uint8_t key_map[MATRIX_ROWS][MATRIX_COLS] = {
    /* Esc F1..F4 F5..F12 Del knob */
    {NONE, SLIDER0, SLIDER0 + 1, SLIDER0 + 2, SLIDER0 + 3,
     PAD0, PAD0 + 1, PAD0 + 2, PAD0 + 3, PAD0 + 4, PAD0 + 5, PAD0 + 6, PAD0 + 7, NONE, NONE},
    /* ` 1 2 3 4 5 6 7 8 9 0 - = Bksp PgUp */
    {NONE, NONE, 13, 15, NONE, 18, 20, 22, NONE, 25, 27, NONE, 30, NONE, NONE},
    /* Tab Q W E R T Y U I O P [ ] \ PgDn */
    {NONE, 12, 14, 16, 17, 19, 21, 23, 24, 26, 28, 29, 31, NONE, NONE},
    /* Caps A S D F G H J K L ; ' Enter Home */
    {NONE, NONE, 1, 3, NONE, 6, 8, 10, NONE, 13, 15, NONE, NONE, NONE, NONE},
    /* LShift _ Z X C V B N M , . _ / RShift Up */
    {NONE, NONE, 0, 2, 4, 5, 7, 9, 11, 12, 14, NONE, 16, NONE, TRANS_UP},
    /* Ctrl Win Alt _ _ _ Space _ _ RAlt Fn RCtrl Left Down Right */
    {NONE, NONE, NONE, NONE, NONE, NONE, SUSTAIN, NONE, NONE, NONE, NONE, NONE, OCT_DOWN, TRANS_DOWN, OCT_UP},
};

typedef enum { K_IDLE, K_ARMED, K_ON } key_phase_t;

typedef struct {
    uint8_t     phase;
    uint8_t     note;       /* note that was sent, so octave shifts never strand a note */
    uint8_t     channel;
    uint8_t     pressure;
    systime_t   armed_at;
    systime_t   on_at;
    systime_t   at_sent_at;
    bool        fn_latch;   /* used with Fn: ignored as a note until released */
    bool        pass;       /* typing key let through in MIDI mode until released */
} midi_key_t;

typedef struct {
    uint8_t   value;        /* last CC value sent */
    uint8_t   ref;          /* travel the key is being held at */
    systime_t ref_at;
} slider_t;

static midi_key_t keys[MATRIX_ROWS][MATRIX_COLS];
static slider_t   sliders[SLIDERS];
static bool       enabled;
static int8_t     octave;
static int8_t     transpose;
static bool       sustain;
static uint8_t    mod_wheel;
static uint8_t    channel_pressure;
static uint8_t    sensitivity = SENSITIVITY_DEFAULT;
static uint8_t    scale;       /* index into scale_masks, 0 = chromatic */
static uint8_t    scale_root;  /* pitch class 0..11 */

/* Notes the host is playing (e.g. a DAW track sent to this port), shown on the keys. */
static uint8_t    remote_notes[16];       /* bitset of 128 notes, melodic channels */
static uint8_t    remote_drums[16];       /* bitset of 128 notes, channel 10 */

static bool bit_get(const uint8_t *set, uint8_t n) {
    return set[n >> 3] & (1 << (n & 7));
}

static void bit_set(uint8_t *set, uint8_t n, bool on) {
    if (on) set[n >> 3] |= (uint8_t)(1 << (n & 7));
    else    set[n >> 3] &= (uint8_t)~(1 << (n & 7));
}

static void remote_note(uint8_t chan, uint8_t note, bool on) {
    bit_set(chan == DRUM_CHANNEL ? remote_drums : remote_notes, note & 0x7F, on);
}

static void remote_note_on(MidiDevice *device, uint8_t status, uint8_t note, uint8_t vel) {
    remote_note(status & 0x0F, note, vel > 0); /* velocity 0 = note off */
}

static void remote_note_off(MidiDevice *device, uint8_t status, uint8_t note, uint8_t vel) {
    remote_note(status & 0x0F, note, false);
}

static void remote_cc(MidiDevice *device, uint8_t chan, uint8_t num, uint8_t val) {
    if (num == 120 || num == 123) { /* all sound / all notes off */
        memset((chan & 0x0F) == DRUM_CHANNEL ? remote_drums : remote_notes, 0, sizeof(remote_notes));
    }
}

void midi_hall_init(void) {
    midi_register_noteon_callback(&midi_device, remote_note_on);
    midi_register_noteoff_callback(&midi_device, remote_note_off);
    midi_register_cc_callback(&midi_device, remote_cc);
}

bool midi_hall_enabled(void) {
    return enabled;
}

static bool fn_held(void) {
    return analog_matrix_get_travel(FN_ROW, FN_COL) >= T_NOTE_ON;
}

static int16_t melodic_note(uint8_t offset) {
    return BASE_NOTE + offset + 12 * octave + transpose;
}

static bool in_scale(int16_t note) {
    uint8_t step = (uint8_t)((note % 12 + 12 - scale_root) % 12);
    return scale_masks[scale] & (1 << step);
}

static uint8_t velocity(uint32_t us) {
    if (us <= VEL_FAST_US) return 127;
    if (us >= VEL_SLOW_US) return 1;
    float x = (logf(VEL_SLOW_US) - logf((float)us)) / (logf(VEL_SLOW_US) - logf(VEL_FAST_US));
    float v = 1.0f + 126.0f * powf(x, sensitivity_gamma[sensitivity]);
    return v > 127.0f ? 127 : (uint8_t)v;
}

static void update_channel_pressure(void) {
    uint8_t max = 0;
    for (uint8_t r = 0; r < MATRIX_ROWS; r++)
        for (uint8_t c = 0; c < MATRIX_COLS; c++)
            if (keys[r][c].phase == K_ON && keys[r][c].channel == HALL_MIDI_CHANNEL && keys[r][c].pressure > max)
                max = keys[r][c].pressure;
    if (max != channel_pressure) {
        channel_pressure = max;
        midi_send_channelpressure(&midi_device, HALL_MIDI_CHANNEL, max);
    }
}

/* note < 0 or > 127 is dropped. Pads pass aftertouch = false. */
static void note_key(midi_key_t *k, int16_t note, uint8_t channel, bool aftertouch, uint8_t travel) {
    systime_t now = chVTGetSystemTimeX();

    switch (k->phase) {
        case K_IDLE:
            if (travel < T_ARM) break;
            k->phase    = K_ARMED;
            k->armed_at = now;
            /* a fast strike can cross both thresholds in one scan */
            __attribute__((fallthrough));
        case K_ARMED:
            if (travel < T_ARM) {
                k->phase = K_IDLE;
            } else if (travel >= T_NOTE_ON) {
                if (note < 0 || note > 127) break;
                k->note       = (uint8_t)note;
                k->channel    = channel;
                k->pressure   = 0;
                k->phase      = K_ON;
                k->at_sent_at = now;
                k->on_at      = now;
                midi_send_noteon(&midi_device, channel, k->note, velocity(TIME_I2US(chTimeDiffX(k->armed_at, now))));
            }
            break;
        case K_ON:
            if (travel < T_NOTE_OFF) {
                midi_send_noteoff(&midi_device, k->channel, k->note, 64);
                k->pressure = 0;
                k->phase    = K_ARMED; /* re-strike from here measures from the release point */
                k->armed_at = now;
                if (aftertouch) update_channel_pressure();
                break;
            }
            if (!aftertouch || TIME_I2US(chTimeDiffX(k->on_at, now)) < AT_DELAY_US) break;
            uint8_t p = travel <= T_AT_START ? 0 : (uint8_t)((uint16_t)(travel - T_AT_START) * 127 / (T_FULL - T_AT_START));
            if (p > 127) p = 127;
            if ((p == 0 || p == 127 || abs((int)p - k->pressure) >= AT_MIN_STEP) && p != k->pressure &&
                TIME_I2US(chTimeDiffX(k->at_sent_at, now)) >= AT_INTERVAL_US) {
                k->pressure   = p;
                k->at_sent_at = now;
                midi_send_aftertouch(&midi_device, k->channel, k->note, p);
                update_channel_pressure();
            }
            break;
    }
}

static void control_key(midi_key_t *k, uint8_t code, uint8_t travel) {
    if (code == SUSTAIN) {
        bool on = sustain ? travel >= SUSTAIN_OFF : travel >= SUSTAIN_ON;
        if (on != sustain) {
            sustain = on;
            midi_send_cc(&midi_device, HALL_MIDI_CHANNEL, 64, on ? 127 : 0);
        }
        return;
    }
    bool pressed = k->phase == K_ON ? travel >= T_NOTE_OFF : travel >= T_NOTE_ON;
    if (pressed == (k->phase == K_ON)) return;
    k->phase = pressed ? K_ON : K_IDLE;
    if (!pressed) return;
    bool fn = fn_held();
    switch (code) {
        case OCT_DOWN:   if (octave > -3) octave--; break;
        case OCT_UP:     if (octave < 3) octave++; break;
        case TRANS_DOWN:
            if (fn) { if (sensitivity > 0) sensitivity--; }
            else if (transpose > -11) transpose--;
            break;
        case TRANS_UP:
            if (fn) { if (sensitivity < SENSITIVITY_LEVELS - 1) sensitivity++; }
            else if (transpose < 11) transpose++;
            break;
    }
}

/* Fn + number row picks the scale, Fn + a note key sets its pitch as the root.
 * Returns true when the key belongs to Fn (also below the press point, so it never arms a note). */
static bool fn_action(midi_key_t *k, uint8_t r, uint8_t c, uint8_t code, uint8_t travel) {
    bool scale_key = r == SCALE_ROW && c >= 1 && c <= SCALES;
    if (!scale_key && code >= PAD0) return false;     /* pads, sliders and controls keep their Fn meaning */
    if (k->phase != K_IDLE) return false;             /* a note started before Fn plays on until released */
    if (travel >= T_NOTE_ON) {
        if (scale_key) scale = c - 1;
        else           scale_root = (uint8_t)((melodic_note(code) % 12 + 12) % 12);
        k->fn_latch = true;
    }
    return true;
}

/* In MIDI mode only Fn and the knob type; Esc and Del pass with Fn (MIDI toggle, bootloader).
 * A key that passes keeps passing until released, so its release is never swallowed. */
static bool passes(midi_key_t *k, uint8_t r, uint8_t c, uint8_t travel) {
    if ((r == FN_ROW && c == FN_COL) || (r == 0 && c == KNOB_COL)) return true;
    if (k->pass) {
        if (travel < T_ARM) k->pass = false;
        return true;
    }
    if (r == 0 && (c == ESC_COL || c == DEL_COL) && fn_held() && travel >= T_ARM) {
        k->pass = true;
        return true;
    }
    return false;
}

bool analog_matrix_midi_hook(analog_key_t *key) {
    if (!enabled) return false;
    uint8_t code = key_map[key->r][key->c];
    midi_key_t *k = &keys[key->r][key->c];
    if (k->fn_latch) {
        if (key->travel < T_NOTE_OFF) k->fn_latch = false;
        return true;
    }
    if (fn_held() && fn_action(k, key->r, key->c, code, key->travel)) return true;
    if (code == NONE) return !passes(k, key->r, key->c, key->travel);
    if (code >= OCT_DOWN)
        control_key(k, code, key->travel);
    else if (code >= SLIDER0)
        ; /* sampled in midi_hall_task(), so a key held still keeps updating */
    else if (code >= PAD0)
        note_key(k, pad_notes[code - PAD0], DRUM_CHANNEL, false, key->travel);
    else {
        int16_t note = melodic_note(code);
        /* out-of-scale keys are silent; a note already sounding is still released */
        if (k->phase != K_ON && !in_scale(note)) note = -1;
        note_key(k, note, HALL_MIDI_CHANNEL, true, key->travel);
    }
    return true;
}

void midi_hall_task(void) {
    if (!enabled) return;
    systime_t now = chVTGetSystemTimeX();
    for (uint8_t i = 0; i < SLIDERS; i++) {
        slider_t *s = &sliders[i];
        uint8_t   t = analog_matrix_get_travel(0, 1 + i);
        if (abs((int)t - s->ref) > SLIDER_JITTER) {
            s->ref    = t;
            s->ref_at = now;
            continue;
        }
        if (t < T_ARM || TIME_I2US(chTimeDiffX(s->ref_at, now)) < SLIDER_HOLD_US) continue;
        int16_t v = t <= SLIDER_MIN ? 0 : (int16_t)(t - SLIDER_MIN) * 127 / (SLIDER_MAX - SLIDER_MIN);
        if (v > 127) v = 127;
        if (v != s->value) {
            s->value = (uint8_t)v;
            midi_send_cc(&midi_device, HALL_MIDI_CHANNEL, slider_cc[i], s->value);
        }
    }
}

void midi_hall_panic(void) {
    for (uint8_t r = 0; r < MATRIX_ROWS; r++)
        for (uint8_t c = 0; c < MATRIX_COLS; c++) {
            midi_key_t *k = &keys[r][c];
            if (k->phase == K_ON && key_map[r][c] < SLIDER0) midi_send_noteoff(&midi_device, k->channel, k->note, 0);
            k->phase    = K_IDLE;
            k->pressure = 0;
            k->fn_latch = false;
        }
    if (sustain) midi_send_cc(&midi_device, HALL_MIDI_CHANNEL, 64, 0);
    sustain = false;
    channel_pressure = 0;
    midi_send_channelpressure(&midi_device, HALL_MIDI_CHANNEL, 0);
    midi_send_cc(&midi_device, HALL_MIDI_CHANNEL, 123, 0); /* all notes off */
    midi_send_cc(&midi_device, DRUM_CHANNEL, 123, 0);
}

void midi_hall_toggle(void) {
    if (enabled) midi_hall_panic();
    enabled = !enabled;
    /* keys held right now (Fn+Esc that switched MIDI on) must still reach their release */
    for (uint8_t r = 0; r < MATRIX_ROWS; r++)
        for (uint8_t c = 0; c < MATRIX_COLS; c++)
            keys[r][c].pass = enabled && analog_matrix_get_travel(r, c) >= T_ARM;
    clear_keyboard(); /* nothing typed before the switch stays held */
}

void midi_hall_mod_wheel(int8_t delta) {
    int16_t v = (int16_t)mod_wheel + delta;
    mod_wheel = v < 0 ? 0 : v > 127 ? 127 : (uint8_t)v;
    midi_send_cc(&midi_device, HALL_MIDI_CHANNEL, 1, mod_wheel);
}

void midi_hall_indicators(uint8_t led_min, uint8_t led_max) {
    if (!enabled) return;
    bool fn = fn_held();
    for (uint8_t r = 0; r < MATRIX_ROWS; r++)
        for (uint8_t c = 0; c < MATRIX_COLS; c++) {
            uint8_t led = g_led_config.matrix_co[r][c];
            if (led == NO_LED || led < led_min || led >= led_max) continue;
            uint8_t code = key_map[r][c];
            midi_key_t *k = &keys[r][c];
            if (fn && r == 0 && c >= 1 && c <= SENSITIVITY_LEVELS) {
                /* while Fn is held, F1..F5 show the velocity sensitivity level */
                bool on = c - 1 <= sensitivity;
                rgb_matrix_set_color(led, on ? 255 : 20, on ? 200 : 20, on ? 0 : 20);
            } else if (fn && r == SCALE_ROW && c >= 1 && c <= SCALES) {
                /* and the number row shows the selected scale */
                bool sel = c - 1 == scale;
                rgb_matrix_set_color(led, sel ? 255 : 30, sel ? 200 : 30, sel ? 0 : 30);
            } else if (code == NONE) {
                rgb_matrix_set_color(led, 0, 0, 0);
            } else if (code >= OCT_DOWN) {
                bool active = code == SUSTAIN ? sustain : k->phase == K_ON;
                rgb_matrix_set_color(led, active ? 255 : 40, active ? 255 : 0, active ? 255 : 80);
            } else if (code >= SLIDER0) {
                /* cyan, brightness = current CC value */
                uint8_t v = 15 + sliders[code - SLIDER0].value * 2 - sliders[code - SLIDER0].value / 8;
                rgb_matrix_set_color(led, 0, v, v);
            } else if (code >= PAD0) {
                if (k->phase == K_ON)                                 rgb_matrix_set_color(led, 255, 40, 40);
                else if (bit_get(remote_drums, pad_notes[code - PAD0])) rgb_matrix_set_color(led, 0, 200, 160);
                else                                                  rgb_matrix_set_color(led, 70, 0, 0);
            } else if (k->phase == K_ON) {
                /* orange, brighter with aftertouch */
                uint8_t v = 120 + k->pressure;
                rgb_matrix_set_color(led, v, v * 2 / 5, 0);
            } else if (melodic_note(code) >= 0 && melodic_note(code) <= 127 && bit_get(remote_notes, (uint8_t)melodic_note(code))) {
                rgb_matrix_set_color(led, 0, 200, 160); /* played by the host */
            } else if (!in_scale(melodic_note(code))) {
                rgb_matrix_set_color(led, 0, 0, 0);
            } else if ((melodic_note(code) % 12 + 12) % 12 == scale_root && scale != 0) {
                rgb_matrix_set_color(led, 0, 160, 40); /* root */
            } else {
                bool black = (1 << (code % 12)) & 0x54A; /* C# D# F# G# A# */
                if (black) rgb_matrix_set_color(led, 0, 0, 90);
                else       rgb_matrix_set_color(led, 90, 90, 90);
            }
        }
}
