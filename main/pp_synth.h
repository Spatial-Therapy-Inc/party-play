// Party Play's sound effects, synthesized (docs/product/party-play-ui.md "声音清单"): modal "struck" sounds (a few
// decaying partials), filtered noise, glides and a soft limiter, the same recipes as the prototype. No samples, so
// nothing to license and nothing in the image. Plain C, host tested (test/run.sh renders them to WAV files).
// Made for the small speaker: energy between 400 Hz and 6 kHz; "low" sounds are harmonics.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "pp_play.h"

#define PP_SYNTH_PRIMS 64  // partials and noise bursts sounding at once (one sound uses 1-30)

typedef struct {
    uint8_t type, wave, filt, env, shape;
    float start;   // seconds to wait before it starts
    float t;       // seconds since it started
    float a, d;    // attack and decay, s
    float peak;
    float f0, f1, glide, phase;
    float fc0, fc1, q;
    float s_lp, s_bp;           // state variable filter
    float form1, form2, f1lp, f1bp, f2lp, f2bp;
    float lp_fc, lp_y;          // one-pole lowpass on tones
    float len;                  // linear-envelope sounds (crowd): total length
} pp_prim_t;

typedef struct {
    pp_prim_t p[PP_SYNTH_PRIMS];
    int n;
    float rate;
    uint32_t rng;
    float master;   // before the limiter
    float last_hit; // seconds of audio rendered at the last collision sound (rate limit, 18 ms)
    double clock;   // seconds of audio rendered
} pp_synth_t;

void pp_synth_init(pp_synth_t *s, int rate, uint32_t seed);
/** Queues a sound. gain: loudness 0..1 for impacts and ticks; length in s for the drum roll and the cheer; the
 *  seat number for PP_SND_SEAT. */
void pp_synth_play(pp_synth_t *s, pp_sound_t sound, float gain);
/** The same, heard `delay` seconds later (placed to the sample: the reaction game's "snap" with its lit frame). */
void pp_synth_play_at(pp_synth_t *s, pp_sound_t sound, float gain, float delay);
/** Renders n mono samples (replacing out). Returns the primitives still sounding or waiting. */
int pp_synth_render(pp_synth_t *s, int16_t *out, int n);
/** The same as floats in -1..1 (yaykit_speak's synth voices). */
int pp_synth_render_f(pp_synth_t *s, float *out, int n);
/** Stops everything at once. */
void pp_synth_clear(pp_synth_t *s);
