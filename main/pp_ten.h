// 十秒挑战 (docs/product/party-play-ui.md): count up, the numbers and the ring fade after 2 s (or 0.6 s blind), stop
// by feel. With seats, everyone takes a turn and the closest wins. Plain C, host tested.
#pragma once

#include <stdbool.h>

#include "pp_seats.h"

typedef enum { PP_TEN_IDLE, PP_TEN_RUN, PP_TEN_SHOWN, PP_TEN_FINAL } pp_ten_state_t;
typedef enum { PP_TEN_PERFECT, PP_TEN_CLOSE, PP_TEN_OK, PP_TEN_AGAIN } pp_ten_rating_t;

enum {
    PP_TEN_EV_SECOND = 1 << 0,  // a whole second passed while the numbers still show (a soft tick)
    PP_TEN_EV_STOP = 1 << 1,    // stopped: `res`, rating
    PP_TEN_EV_NEXT = 1 << 2,    // multi: next player's turn (`turn`)
    PP_TEN_EV_FINAL = 1 << 3,   // multi: everyone went; `winner` was closest
};

typedef struct {
    pp_ten_state_t st;
    int mode;           // 0: visible for the first 3 s, 1: blind
    float t, res, shown_t;
    int last_sec;
    bool multi;
    int n, turn, winner;
    float results[PP_SEATS_MAX];
    bool has[PP_SEATS_MAX];
    float best;         // solo best this session (closest to 10), < 0 none
} pp_ten_t;

void pp_ten_init(pp_ten_t *t, int players);  // players < 2: solo
/** Tap: start (IDLE), stop (RUN), back to IDLE (SHOWN, solo) or a new round (FINAL). */
int pp_ten_tap(pp_ten_t *t);
int pp_ten_update(pp_ten_t *t, float dt);
/** How visible the running numbers and ring are, 0..1. */
float pp_ten_visible(const pp_ten_t *t);
pp_ten_rating_t pp_ten_rate(float seconds);
