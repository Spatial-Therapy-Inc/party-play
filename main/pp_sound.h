// Party Play's sounds through yaykit_speak's effect mixer (P13): one synth voice (yk_sfx_play_synth) renders the whole
// pp_synth mix every 5 ms block; the game queues sounds to it without locks. A sound can be placed to the sample on
// the speaker's clock (the reaction game's "snap" with its lit frame). At 24 kHz with 5 ms blocks a sound is heard
// about 25-45 ms after it is asked for (P13's 50 ms).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "pp_play.h"

/** Starts the speaker in low-latency mode (yk_speak_init_cfg: 24 kHz, 5 ms blocks) and the synth. */
esp_err_t pp_sound_start(void);
/** Queues a sound (any task). at_us: esp_timer time it should be heard, 0 = as soon as possible. */
void pp_sound_play(pp_sound_t sound, float gain, int64_t at_us);
/** Silence now and drop what is queued (face down, leaving: a 10 ms fade); false lets sounds play again. */
void pp_sound_mute(bool mute);
/** In a game the amplifier stays on: a sound can come any moment and must not wait the 300 ms it takes to wake. */
void pp_sound_hold_amp(bool on);
/** How long a sound asked for now takes to be heard, ms. */
int pp_sound_latency_ms(void);
