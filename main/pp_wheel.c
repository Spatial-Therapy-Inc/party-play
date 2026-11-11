#include "pp_wheel.h"

#include "pp_util.h"

void pp_wheel_init(pp_wheel_t *w, const pp_seats_t *seats, uint32_t seed) {
    *w = (pp_wheel_t){.th = -PP_PI / 2, .dir = 1, .winner = -1, .rng = seed ? seed : 7};
    w->under = pp_seats_nearest(seats, w->th);
}

void pp_wheel_spin(pp_wheel_t *w, const pp_seats_t *seats, float speed) {
    if (seats->n < 1) return;
    w->winner = pp_randi(&w->rng, seats->n);
    float lo, hi;
    pp_seats_sector(seats, w->winner, &lo, &hi);
    const float target = lo + (hi - lo) * pp_randr(&w->rng, 0.15f, 0.85f);
    w->dir = speed < 0 ? -1 : 1;
    float w0 = pp_clampf(fabsf(speed), 10, 30);
    const float base = w->dir > 0 ? pp_ang_p(target - w->th) : pp_ang_p(w->th - target);
    int k = (int)lroundf((w0 * 4.8f / 2 - base) / PP_TAU);
    if (k < 2) k = 2;
    const float total = base + k * PP_TAU;
    float T = 2 * total / w0;
    if (T > 7) T = 7, w0 = 2 * total / T;
    if (T < 3.6f) T = 3.6f, w0 = 2 * total / T;
    w->th0 = w->th;
    w->w0 = w0;
    w->T = T;
    w->t = 0;
    w->spinning = true;
    w->drummed = false;
}

static int crossing(pp_wheel_t *w, const pp_seats_t *seats, float gain) {
    const int u = pp_seats_nearest(seats, w->th);
    if (u == w->under) return 0;
    w->under = u;
    w->tick_gain = gain;
    return PP_WHEEL_TICK;
}

int pp_wheel_drag(pp_wheel_t *w, const pp_seats_t *seats, float da) {
    if (w->spinning) return 0;
    w->th = pp_ang_n(w->th + da);
    return crossing(w, seats, 1);
}

float pp_wheel_speed(const pp_wheel_t *w) { return w->spinning ? w->w0 * (1 - w->t / w->T) : 0; }

int pp_wheel_update(pp_wheel_t *w, const pp_seats_t *seats, float dt) {
    if (!w->spinning) return 0;
    w->t += dt;
    const float t = w->t < w->T ? w->t : w->T;
    const float speed = w->w0 * (1 - t / w->T);
    w->th = w->th0 + w->dir * (w->w0 * t - w->w0 * t * t / (2 * w->T));
    int ev = crossing(w, seats, pp_clampf(0.35f + speed / 16, 0.35f, 1));
    if (!w->drummed && w->T - w->t < 1.35f) {
        w->drummed = true;
        w->drum_s = w->T - w->t;
        ev |= PP_WHEEL_DRUM;
    }
    if (w->t >= w->T) {
        w->spinning = false;
        w->th = pp_ang_n(w->th);
        ev |= PP_WHEEL_DONE;
    }
    return ev;
}
