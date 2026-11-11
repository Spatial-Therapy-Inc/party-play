// 传炸弹 (docs/product/party-play-ui.md): it goes off at a hidden moment 2-50 s after it is lit, with about the same
// chance every second, so it can go off while the ticking is still slow (user, 2026-11-10: players heard how near it
// was and stopped hurrying). The ticking and the fuse on screen run on a clock of their own that never looks at that
// moment: slow to fast over 25 s, then fast until it goes. Held still for more than 3 s it nags, and both the hidden
// fuse and the ticking go 1.6 times faster; then it goes off and the person holding it gets a small forfeit, one of
// 20, none repeated until all have come up. Plain C, host tested.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define PP_PENALTY_COUNT 20

typedef enum { PP_BOMB_IDLE, PP_BOMB_LIT, PP_BOMB_BOOM, PP_BOMB_PENALTY, PP_BOMB_AFTER } pp_bomb_state_t;

enum {
    PP_BOMB_EV_TICK = 1 << 0,     // `alt` says tick or tock; `urging` makes it louder
    PP_BOMB_EV_BEEP = 1 << 1,     // the nag beep, every 1.2 s while still
    PP_BOMB_EV_NAG = 1 << 2,      // Yay says "pass it" (at most every 5 s)
    PP_BOMB_EV_BOOM = 1 << 3,
    PP_BOMB_EV_PENALTY = 1 << 4,  // `penalty` is the forfeit to show
};

typedef struct {
    const char *zh_short, *zh_say, *en_short, *en_say;  // short: on screen (<= 8 characters in Chinese)
} pp_penalty_t;

/** The forfeits: everything can be done sitting down (hard rule), family friendly, no food or drink, no touching,
 *  nobody embarrassed, done within ten seconds. Reviewed by the main session on 2026-10-20. */
extern const pp_penalty_t PP_PENALTIES[PP_PENALTY_COUNT];

typedef struct {
    pp_bomb_state_t st;
    float p;      // the hidden fuse burnt, 0..1: it goes off at 1. Nothing seen or heard may follow it.
    float T;      // the hidden length, s
    float ramp;   // the ticking's own clock, 0..1 (and the fuse drawn on screen): never tied to p or T
    float t;      // in this state
    float tick, still, beep, nag_cool;
    bool alt, urging;
    bool quick;   // lit in quick mode: the ticking ramps over 8 s, not 25 s
    int penalty;  // index into PP_PENALTIES, -1 none
    uint8_t bag[PP_PENALTY_COUNT];
    int bag_n;
    uint32_t rng;
} pp_bomb_t;

void pp_bomb_init(pp_bomb_t *b, uint32_t seed);
/** Lights the fuse (IDLE), or brings a new bomb (PENALTY / AFTER). quick: 1-13 s instead of 2-50 s (console). */
void pp_bomb_tap(pp_bomb_t *b, bool quick);
/** Pinches the fuse out (side key while lit) or skips the forfeit. True if something happened. */
bool pp_bomb_cancel(pp_bomb_t *b);
/** moving: the device moved within the last second (yaykit_motion). penalties: the setting. */
int pp_bomb_update(pp_bomb_t *b, float dt, bool moving, bool penalties);
/** The next forfeit: a shuffled bag of all 20, refilled when empty. */
int pp_bomb_next_penalty(pp_bomb_t *b);
