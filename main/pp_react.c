#include "pp_react.h"

#include <string.h>

#include "pp_util.h"

void pp_react_init(pp_react_t *r, int players, uint32_t seed) {
    memset(r, 0, sizeof(*r));
    r->n = pp_clampi(players, 2, PP_SEATS_MAX);
    r->rng = seed ? seed : 11;
    r->winner = r->last_seat = -1;
    r->st = PP_RX_READY;
}

static void new_round(pp_react_t *r) {
    r->st = PP_RX_WAIT;
    r->t = 0;
    r->beat = 0;
    r->wait = pp_randr(&r->rng, 1.8f, 4.6f);
    r->decoy_done = r->round < 2 || pp_randf(&r->rng) > 0.35f;  // decoys from the third round
    r->round++;
    memset(r->locked, 0, sizeof(r->locked));
    r->winner = -1;
}

int pp_react_start(pp_react_t *r) {
    if (r->st == PP_RX_OVER) {
        memset(r->score, 0, sizeof(r->score));
        r->round = 0;
        r->st = PP_RX_READY;
        return 0;
    }
    if (r->st != PP_RX_READY) return 0;
    new_round(r);
    return 0;
}

int pp_react_update(pp_react_t *r, float dt) {
    r->t += dt;
    switch (r->st) {
    case PP_RX_WAIT:
        r->beat += dt;
        if (r->t >= r->wait) {
            r->t = 0;
            if (!r->decoy_done) {
                r->decoy_done = true;
                r->st = PP_RX_DECOY;
                return PP_RX_EV_DECOY;
            }
            r->st = PP_RX_GO;
            return PP_RX_EV_GO;
        }
        if (r->beat >= 0.9f) {
            r->beat = 0;
            return PP_RX_EV_HEART;
        }
        return 0;
    case PP_RX_DECOY:
        if (r->t >= 0.45f) {
            r->st = PP_RX_WAIT;
            r->t = 0;
            r->wait = pp_randr(&r->rng, 1.0f, 2.4f);
        }
        return 0;
    case PP_RX_GO:
        if (r->t >= PP_REACT_GO_S) {
            r->st = PP_RX_WAIT;
            r->t = 0;
            r->wait = pp_randr(&r->rng, 1.5f, 3.0f);
            return PP_RX_EV_NOBODY;
        }
        return 0;
    case PP_RX_WIN:
        if (r->t >= 1.7f) {
            if (r->score[r->winner] >= PP_REACT_TARGET) {
                r->st = PP_RX_OVER;
                r->t = 0;
                return PP_RX_EV_OVER;
            }
            new_round(r);
        }
        return 0;
    case PP_RX_TIE:
        if (r->t >= 1.4f) new_round(r);
        return 0;
    default:
        return 0;
    }
}

int pp_react_slap(pp_react_t *r, int seat, float rt) {
    if (seat < 0 || seat >= r->n || r->locked[seat]) return 0;
    if (r->st == PP_RX_WAIT || r->st == PP_RX_DECOY) {
        r->locked[seat] = true;
        r->last_seat = seat;
        int ev = PP_RX_EV_FALSE;
        bool all = true;
        for (int i = 0; i < r->n; i++) all = all && r->locked[i];
        if (all) {
            new_round(r);
            r->round--;  // the void round does not count towards the decoys
            ev |= PP_RX_EV_VOID;
        }
        return ev;
    }
    if (r->st != PP_RX_GO) return 0;
    r->st = PP_RX_WIN;
    r->t = 0;
    r->winner = seat;
    r->rt = rt;
    if (r->score[seat] < 255) r->score[seat]++;
    return PP_RX_EV_WIN;
}

int pp_react_tie(pp_react_t *r) {
    if (r->st != PP_RX_GO) return 0;
    r->st = PP_RX_TIE;
    r->t = 0;
    return PP_RX_EV_TIE;
}
