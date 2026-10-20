/* Hall effect MIDI mode for the Keychron Q1 HE. See midi_hall.h. */
#include QMK_KEYBOARD_H
#include <ch.h>
#include <math.h>
#include "qmk_midi.h"
#include "analog_matrix.h"
#include "analog_matrix_type.h"
#include "midi_hall.h"

/* Travel is reported in 1/60 mm (0.1 mm = 6), 240 = bottomed out. */
#define T_ARM       18   /* 0.3 mm: start timing the strike          */
#define T_NOTE_ON   90   /* 1.5 mm: note on, velocity from arm..here */
#define T_NOTE_OFF  54   /* 0.9 mm: note off (hysteresis)            */
#define T_AT_START  180  /* 3.0 mm: aftertouch starts                */
#define T_FULL      240

/* Velocity: log of the arm..note-on time mapped to 0..1, then shaped by the sensitivity curve. */
#define VEL_FAST_US 2500.0f   /* strike this fast or faster = 127 */
#define VEL_SLOW_US 100000.0f /* strike this slow or slower = 1   */
/* Fn + Up/Down in MIDI mode; < 1 = softer touch reaches loud notes, > 1 = harder. */
static const float sensitivity_gamma[] = {1.6f, 1.25f, 1.0f, 0.8f, 0.6f};
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
#define SLIDER_MIN     24     /* 0.4 mm = 0   */
#define SLIDER_MAX     216    /* 3.6 mm = 127 */
#define SLIDER_JITTER  3
#define SLIDER_HOLD_US 30000  /* a release is faster than this, so it does not move the value */

#define FN_ROW 5
#define FN_COL 10

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

bool midi_hall_enabled(void) {
    return enabled;
}

static bool fn_held(void) {
    return analog_matrix_get_travel(FN_ROW, FN_COL) >= T_NOTE_ON;
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

bool analog_matrix_midi_hook(analog_key_t *key) {
    if (!enabled) return false;
    uint8_t code = key_map[key->r][key->c];
    if (code == NONE) return false;
    midi_key_t *k = &keys[key->r][key->c];
    if (code >= OCT_DOWN)
        control_key(k, code, key->travel);
    else if (code >= SLIDER0)
        ; /* sampled in midi_hall_task(), so a key held still keeps updating */
    else if (code >= PAD0)
        note_key(k, pad_notes[code - PAD0], DRUM_CHANNEL, false, key->travel);
    else
        note_key(k, BASE_NOTE + code + 12 * octave + transpose, HALL_MIDI_CHANNEL, true, key->travel);
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
                if (k->phase == K_ON) rgb_matrix_set_color(led, 255, 40, 40);
                else                  rgb_matrix_set_color(led, 70, 0, 0);
            } else if (k->phase == K_ON) {
                /* orange, brighter with aftertouch */
                uint8_t v = 120 + k->pressure;
                rgb_matrix_set_color(led, v, v * 2 / 5, 0);
            } else {
                bool black = (1 << (code % 12)) & 0x54A; /* C# D# F# G# A# */
                if (black) rgb_matrix_set_color(led, 0, 0, 90);
                else       rgb_matrix_set_color(led, 90, 90, 90);
            }
        }
}
