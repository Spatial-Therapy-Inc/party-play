// Seats around the table (docs/product/party-play-ui.md "座位和颜色"): where each player sits, their colour and their
// slice of the edge. Plain C, host tested.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define PP_SEATS_MAX 8
#define PP_SEAT_TOGGLE_RAD 0.33f  // a tap this close to a seat (about 19 degrees) takes it away

/** The eight colours, given in seat order from the side key's edge clockwise: odd seats darker, even ones lighter,
 *  so neighbours always differ in lightness (round-screen-design.md 3). The seat badge also carries 1-8 dots. */
extern const uint32_t PP_SEAT_COLORS[PP_SEATS_MAX];

typedef struct {
    float a;        // angle of the seat (pp_util.h convention)
    uint8_t color;  // index into PP_SEAT_COLORS
} pp_seat_t;

typedef struct {
    pp_seat_t s[PP_SEATS_MAX];
    int n;
    bool tapped;  // the players tapped their own places (otherwise spread evenly)
} pp_seats_t;

/** n seats spread evenly, the first in front of the side key's edge. n is clamped to 2-8. */
void pp_seats_even(pp_seats_t *seats, int n);
/** Sorts by angle from the side key's edge clockwise and hands out the colours in that order. */
void pp_seats_sort_color(pp_seats_t *seats);
/** A tap on the edge while seating: +1 added, -1 removed (it was near a seat), 0 full (8 already). Unsorted. */
int pp_seats_toggle(pp_seats_t *seats, float a);
/** The seat whose slice holds angle a (nearest seat); -1 when there are none. */
int pp_seats_nearest(const pp_seats_t *seats, float a);
/** Seat i's slice: from lo to hi (hi > lo, hi - lo <= 2 pi), bisecting the gaps to its neighbours. */
void pp_seats_sector(const pp_seats_t *seats, int i, float *lo, float *hi);
/** Index of seat i among the seats sorted from the side key's edge (its badge number - 1). */
static inline int pp_seat_number(const pp_seats_t *seats, int i) { return seats->s[i].color + 1; }
