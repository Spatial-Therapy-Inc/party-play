#include "pp_seats.h"

#include "pp_util.h"

const uint32_t PP_SEAT_COLORS[PP_SEATS_MAX] = {
    0xDF795D,  // 1 coral   (dark)
    0xF3E6C8,  // 2 cream   (light)
    0xB7654A,  // 3 clay    (dark)
    0x9FB38C,  // 4 sage    (light)
    0xB98AA6,  // 5 plum    (dark)
    0xE3A857,  // 6 amber   (light)
    0x808B4F,  // 7 olive   (dark; darkened from the draft so the lights and darks alternate)
    0xCDB18E,  // 8 sand    (light)
};

// measured clockwise from the side key's edge; a seat right at the edge (within rounding) counts as the first
static float from_key(float a) { return pp_ang_p(a - PP_PI / 2 + 1e-4f); }

void pp_seats_even(pp_seats_t *seats, int n) {
    n = pp_clampi(n, 2, PP_SEATS_MAX);
    seats->n = n;
    seats->tapped = false;
    for (int i = 0; i < n; i++) seats->s[i].a = pp_ang_n(PP_PI / 2 + i * PP_TAU / n);
    pp_seats_sort_color(seats);
}

void pp_seats_sort_color(pp_seats_t *seats) {
    for (int i = 1; i < seats->n; i++) {  // insertion sort, at most 8
        pp_seat_t v = seats->s[i];
        int j = i - 1;
        while (j >= 0 && from_key(seats->s[j].a) > from_key(v.a)) {
            seats->s[j + 1] = seats->s[j];
            j--;
        }
        seats->s[j + 1] = v;
    }
    for (int i = 0; i < seats->n; i++) seats->s[i].color = (uint8_t)i;
}

int pp_seats_toggle(pp_seats_t *seats, float a) {
    for (int i = 0; i < seats->n; i++) {
        if (fabsf(pp_ang_n(seats->s[i].a - a)) < PP_SEAT_TOGGLE_RAD) {
            for (int j = i; j < seats->n - 1; j++) seats->s[j] = seats->s[j + 1];
            seats->n--;
            return -1;
        }
    }
    if (seats->n >= PP_SEATS_MAX) return 0;
    seats->s[seats->n].a = pp_ang_n(a);
    seats->s[seats->n].color = (uint8_t)seats->n;
    seats->n++;
    return 1;
}

int pp_seats_nearest(const pp_seats_t *seats, float a) {
    int best = -1;
    float bd = 99;
    for (int i = 0; i < seats->n; i++) {
        const float d = fabsf(pp_ang_n(a - seats->s[i].a));
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}

void pp_seats_sector(const pp_seats_t *seats, int i, float *lo, float *hi) {
    const int n = seats->n;
    const float a = seats->s[i].a;
    if (n <= 1) {
        *lo = a - PP_PI;
        *hi = a + PP_PI;
        return;
    }
    // neighbours by angle, whatever the array order
    float prev_gap = PP_TAU, next_gap = PP_TAU;
    for (int j = 0; j < n; j++) {
        if (j == i) continue;
        const float fwd = pp_ang_p(seats->s[j].a - a), back = pp_ang_p(a - seats->s[j].a);
        if (fwd > 0 && fwd < next_gap) next_gap = fwd;
        if (back > 0 && back < prev_gap) prev_gap = back;
    }
    *lo = a - prev_gap / 2;
    *hi = a + next_gap / 2;
}
