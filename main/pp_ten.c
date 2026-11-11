#include "pp_ten.h"

#include <math.h>
#include <string.h>

void pp_ten_init(pp_ten_t *t, int players) {
    const int mode = t->mode;
    const float best = t->best;
    memset(t, 0, sizeof(*t));
    t->mode = mode;
    t->best = best > 0 ? best : -1;
    t->multi = players >= 2;
    t->n = t->multi ? (players > PP_SEATS_MAX ? PP_SEATS_MAX : players) : 1;
    t->winner = -1;
}

pp_ten_rating_t pp_ten_rate(float seconds) {
    const float d = fabsf(seconds - 10);
    if (d <= 0.05f) return PP_TEN_PERFECT;
    if (d <= 0.2f) return PP_TEN_CLOSE;
    if (d <= 0.5f) return PP_TEN_OK;
    return PP_TEN_AGAIN;
}

static int stop(pp_ten_t *t) {
    t->st = PP_TEN_SHOWN;
    t->res = t->t;
    t->shown_t = 0;
    if (t->multi) {
        t->results[t->turn] = t->res;
        t->has[t->turn] = true;
    } else if (t->best < 0 || fabsf(t->res - 10) < fabsf(t->best - 10)) {
        t->best = t->res;
    }
    return PP_TEN_EV_STOP;
}

int pp_ten_tap(pp_ten_t *t) {
    switch (t->st) {
    case PP_TEN_IDLE:
        t->st = PP_TEN_RUN;
        t->t = 0;
        t->last_sec = 0;
        return 0;
    case PP_TEN_RUN:
        return stop(t);
    case PP_TEN_SHOWN:
        if (!t->multi) t->st = PP_TEN_IDLE;
        return 0;
    case PP_TEN_FINAL: {
        const int n = t->n;
        pp_ten_init(t, n);
        return 0;
    }
    }
    return 0;
}

float pp_ten_visible(const pp_ten_t *t) {
    if (t->mode == 1) return t->t < 0.6f ? 1 - t->t / 0.6f : 0;
    if (t->t < 2) return 1;
    const float v = 1 - (t->t - 2);
    return v > 0 ? v : 0;
}

int pp_ten_update(pp_ten_t *t, float dt) {
    int ev = 0;
    if (t->st == PP_TEN_RUN) {
        t->t += dt;
        const int sec = (int)t->t;
        if (sec > t->last_sec) {
            t->last_sec = sec;
            if (pp_ten_visible(t) > 0.3f) ev |= PP_TEN_EV_SECOND;
        }
        if (t->t > 20) ev |= stop(t);  // nobody stopped it
    } else if (t->st == PP_TEN_SHOWN && t->multi) {
        t->shown_t += dt;
        if (t->shown_t > 1.9f) {
            t->turn++;
            if (t->turn >= t->n) {
                float bd = 1e9f;
                for (int i = 0; i < t->n; i++) {
                    const float d = fabsf(t->results[i] - 10);
                    if (t->has[i] && d < bd) bd = d, t->winner = i;
                }
                t->st = PP_TEN_FINAL;
                ev |= PP_TEN_EV_FINAL;
            } else {
                t->st = PP_TEN_IDLE;
                ev |= PP_TEN_EV_NEXT;
            }
        }
    } else if (t->st == PP_TEN_SHOWN) {
        t->shown_t += dt;
    }
    return ev;
}
