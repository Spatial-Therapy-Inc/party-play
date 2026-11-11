// 抢拍, the reaction game (docs/product/party-play-ui.md): wait a random 1.8-4.6 s, then the screen lights up and the
// first to slap their own slice scores; slapping early is a false start; from round 3 a decoy flash may come first.
// First to 3 wins. Plain C, host tested; the device measures the reaction time from the frame that lit up.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "pp_seats.h"

#define PP_REACT_TARGET 3
#define PP_REACT_GO_S 3.0f  // the lit frame stays this long for someone to slap

typedef enum {
    PP_RX_READY,  // waiting for the start key in the middle
    PP_RX_WAIT,   // heartbeat; slapping now is a false start
    PP_RX_DECOY,  // the sage flash; slapping now is a false start too
    PP_RX_GO,     // lit: first slap wins
    PP_RX_WIN,    // someone scored this round
    PP_RX_TIE,    // two at once: replay
    PP_RX_OVER,   // someone reached 3
} pp_rx_state_t;

enum {
    PP_RX_EV_HEART = 1 << 0,
    PP_RX_EV_DECOY = 1 << 1,
    PP_RX_EV_GO = 1 << 2,
    PP_RX_EV_FALSE = 1 << 3,   // a false start by `last_seat`
    PP_RX_EV_WIN = 1 << 4,     // `winner` scored after `rt` seconds
    PP_RX_EV_TIE = 1 << 5,
    PP_RX_EV_NOBODY = 1 << 6,  // nobody slapped within 3 s
    PP_RX_EV_OVER = 1 << 7,    // `winner` won the match
    PP_RX_EV_VOID = 1 << 8,    // everybody false-started: the round starts again
};

typedef struct {
    pp_rx_state_t st;
    int n;
    float t, wait, beat;
    bool decoy_done;
    int round;
    bool locked[PP_SEATS_MAX];
    uint8_t score[PP_SEATS_MAX];
    int winner, last_seat;
    float rt;
    uint32_t rng;
} pp_react_t;

void pp_react_init(pp_react_t *r, int players, uint32_t seed);
/** The start key: begins the first round (READY) or a new match (OVER). */
int pp_react_start(pp_react_t *r);
/** Time passes. */
int pp_react_update(pp_react_t *r, float dt);
/** Seat `seat` slapped its slice; rt is the device's measured time since the lit frame (used in GO). */
int pp_react_slap(pp_react_t *r, int seat, float rt);
/** The touch controller reported two people at once during GO (its point jumped): no point, replay. */
int pp_react_tie(pp_react_t *r);
