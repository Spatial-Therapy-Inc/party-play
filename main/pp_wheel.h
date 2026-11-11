// The spinner (docs/product/party-play.md "游戏逻辑"): seats stay at the edge, the needle in the middle spins and
// stops pointing at one person. Fair: the person is drawn first (uniformly, whatever the slice sizes), then a stop
// inside the middle 70% of their slice, then a constant deceleration that lands exactly there. Plain C, host tested.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "pp_seats.h"

enum {
    PP_WHEEL_TICK = 1 << 0,   // crossed into another slice (tick_gain says how loud)
    PP_WHEEL_DRUM = 1 << 1,   // the last 1.35 s: start the drum roll (drum_s long)
    PP_WHEEL_DONE = 1 << 2,   // stopped on `winner`
};

typedef struct {
    float th;       // needle angle
    float th0, w0, T, t;
    int dir;
    bool spinning;
    bool drummed;
    int winner;     // valid once DONE
    int under;      // seat under the needle (for the ticks)
    float tick_gain, drum_s;
    uint32_t rng;
} pp_wheel_t;

void pp_wheel_init(pp_wheel_t *w, const pp_seats_t *seats, uint32_t seed);
/** Starts a spin with the flick's angular speed (rad/s; sign = direction). |speed| is kept within 10-30 rad/s and
 *  the spin lasts 3.6-7 s. */
void pp_wheel_spin(pp_wheel_t *w, const pp_seats_t *seats, float speed);
/** The finger drags the needle (not spinning): moves it by da and reports a tick when it crosses a slice. */
int pp_wheel_drag(pp_wheel_t *w, const pp_seats_t *seats, float da);
/** Advances a spin by dt; returns PP_WHEEL_* flags. */
int pp_wheel_update(pp_wheel_t *w, const pp_seats_t *seats, float dt);
/** Current angular speed, rad/s (0 when not spinning). */
float pp_wheel_speed(const pp_wheel_t *w);
