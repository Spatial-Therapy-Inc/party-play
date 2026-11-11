// Touch gestures on the round screen (docs/product/party-play.md "触摸"): tap (moved < 20 px, < 600 ms); a drag
// that starts at radius >= rim_min turns along the edge (with the flick's angular speed from the last 100 ms);
// other drags are swipes of >= 60 px, read in the frame of whoever the middle faces, so the person opposite swiping
// to their left is a left swipe too. Plain C, host tested; coordinates are screen pixels (0..466).
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    PP_G_NONE,
    PP_G_TAP,
    PP_G_RIM_START,
    PP_G_RIM_MOVE,  // `da` holds the angle turned since the last move
    PP_G_RIM_END,   // `w` holds the angular speed at release, rad/s
    PP_G_SWIPE_LEFT,
    PP_G_SWIPE_RIGHT,
    PP_G_SWIPE_DOWN,
    PP_G_SWIPE_UP,
} pp_gesture_ev_t;

typedef struct {
    float t;  // seconds
    float a;  // unwrapped angle turned so far
} pp_g_sample_t;

typedef struct {
    bool down, moved, rim;
    float x0, y0, t0, r0, a_prev, acc;
    float rim_min;
    pp_g_sample_t s[16];
    int ns;
    // outputs of the last call
    float da, w;
    float x, y;  // where it happened (tap point, rim point)
} pp_gesture_t;

void pp_gesture_down(pp_gesture_t *g, float x, float y, float t, float rim_min);
pp_gesture_ev_t pp_gesture_move(pp_gesture_t *g, float x, float y, float t);
/** reader_rot: rotation of the middle content (pp_play view), to read swipes in that frame. */
pp_gesture_ev_t pp_gesture_up(pp_gesture_t *g, float x, float y, float t, float reader_rot);

/** Angle and radius of a screen point around the centre. */
float pp_point_angle(float x, float y);
float pp_point_radius(float x, float y);
