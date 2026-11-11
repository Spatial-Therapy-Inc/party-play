// yaykit_motion (P12) in the game's terms: its events and state become pp_motion_in_t. Plain C, host tested with
// the component's core (yk_motion_core.c) fed synthetic samples.
#pragma once

#include <stdbool.h>

#include "pp_play.h"
#include "yaykit_motion.h"

#define PP_DICE_PUSH 3000.0f  // px/s^2 on the dice per g of tilt or shake
#define PP_DICE_DEADBAND 0.05f  // g: a table is never quite level, nor a sensor quite zero

/** yaykit_motion's angles (degrees clockwise from 12 o'clock) in the game's (radians clockwise from 3 o'clock). */
float pp_angle_from_motion(float deg);

/** events: what came since the last frame; st: the state now. have_sensor false: no motion at all (the bomb then
 *  counts as always moving, so it never nags). */
void pp_motion_map(const yk_motion_event_t *events, int n, const yk_motion_state_t *st, bool have_sensor,
                   pp_motion_in_t *out);
