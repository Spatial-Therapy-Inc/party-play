#include "pp_gesture.h"

#include <string.h>

#include "pp_util.h"

#define TAP_MOVE 20.0f
#define TAP_TIME 0.6f
#define DRAG_START 12.0f
#define SWIPE_MIN 60.0f
#define FLICK_WINDOW 0.1f

float pp_point_angle(float x, float y) { return atan2f(y - PP_C, x - PP_C); }
float pp_point_radius(float x, float y) { return sqrtf((x - PP_C) * (x - PP_C) + (y - PP_C) * (y - PP_C)); }

static void sample(pp_gesture_t *g, float t) {
    if (g->ns == 16) {
        memmove(g->s, g->s + 1, sizeof(g->s[0]) * 15);
        g->ns = 15;
    }
    g->s[g->ns++] = (pp_g_sample_t){t, g->acc};
    while (g->ns > 2 && t - g->s[0].t > FLICK_WINDOW) {
        memmove(g->s, g->s + 1, sizeof(g->s[0]) * (g->ns - 1));
        g->ns--;
    }
}

void pp_gesture_down(pp_gesture_t *g, float x, float y, float t, float rim_min) {
    memset(g, 0, sizeof(*g));
    g->down = true;
    g->x0 = g->x = x;
    g->y0 = g->y = y;
    g->t0 = t;
    g->r0 = pp_point_radius(x, y);
    g->a_prev = pp_point_angle(x, y);
    g->rim_min = rim_min;
    sample(g, t);
}

pp_gesture_ev_t pp_gesture_move(pp_gesture_t *g, float x, float y, float t) {
    if (!g->down) return PP_G_NONE;
    g->x = x;
    g->y = y;
    pp_gesture_ev_t ev = PP_G_NONE;
    if (!g->moved && hypotf(x - g->x0, y - g->y0) > DRAG_START) {
        g->moved = true;
        g->rim = g->r0 >= g->rim_min;
        if (g->rim) ev = PP_G_RIM_START;
    }
    if (g->rim) {
        const float a = pp_point_angle(x, y);
        g->da = pp_ang_n(a - g->a_prev);
        g->a_prev = a;
        g->acc += g->da;
        sample(g, t);
        if (ev == PP_G_NONE) ev = PP_G_RIM_MOVE;
    }
    return ev;
}

pp_gesture_ev_t pp_gesture_up(pp_gesture_t *g, float x, float y, float t, float reader_rot) {
    if (!g->down) return PP_G_NONE;
    g->down = false;
    g->x = x;
    g->y = y;
    const float dx = x - g->x0, dy = y - g->y0;
    if (!g->moved && hypotf(dx, dy) < TAP_MOVE && t - g->t0 < TAP_TIME) return PP_G_TAP;
    if (g->rim) {
        const pp_g_sample_t a = g->s[0], b = g->s[g->ns - 1];
        g->w = b.t > a.t ? (b.a - a.a) / (b.t - a.t) : 0;
        return PP_G_RIM_END;
    }
    if (!g->moved) return PP_G_NONE;
    // into the reader's frame: undo the rotation of the middle
    const float c = cosf(-reader_rot), s = sinf(-reader_rot);
    const float rx = dx * c - dy * s, ry = dx * s + dy * c;
    if (fabsf(rx) >= SWIPE_MIN && fabsf(rx) > fabsf(ry) * 1.2f) return rx < 0 ? PP_G_SWIPE_LEFT : PP_G_SWIPE_RIGHT;
    if (fabsf(ry) >= SWIPE_MIN && fabsf(ry) > fabsf(rx) * 1.2f) return ry > 0 ? PP_G_SWIPE_DOWN : PP_G_SWIPE_UP;
    return PP_G_NONE;
}
