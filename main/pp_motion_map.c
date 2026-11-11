#include "pp_motion_map.h"

#include <math.h>
#include <string.h>

#include "pp_util.h"

float pp_angle_from_motion(float deg) { return pp_ang_n((deg - 90) * PP_PI / 180); }

void pp_motion_map(const yk_motion_event_t *events, int n, const yk_motion_state_t *st, bool have_sensor,
                   pp_motion_in_t *out) {
    memset(out, 0, sizeof(*out));
    out->holder = PP_PI / 2;  // the side key's edge until someone tilts it
    if (!have_sensor || !st) {
        out->moving = true;
        return;
    }
    // A free die feels gravity's part in the plane of the glass (it slides downhill when tilted) minus the device's
    // own acceleration (it lags behind a shake).
    float fx = st->now.gravity[0] - st->now.lin[0], fy = st->now.gravity[1] - st->now.lin[1];
    if (fabsf(fx) < PP_DICE_DEADBAND) fx = 0;
    if (fabsf(fy) < PP_DICE_DEADBAND) fy = 0;
    out->fx = fx * PP_DICE_PUSH;
    out->fy = fy * PP_DICE_PUSH;
    const float lz = fabsf(st->now.lin[2]);
    out->jolt = lz > 0.6f ? pp_clampf(lz - 0.6f, 0, 1) : 0;  // a knock along the glass's normal flips a die
    out->held = !st->still || st->shaking;  // dice land only once it has been at rest for still_ms
    out->moving = st->moving;
    out->holder = pp_angle_from_motion(st->holder_angle);
    out->holder_conf = st->holder_confidence;
    if (st->shaking) out->events |= PP_MO_SHAKE;
    for (int i = 0; i < n; i++) {
        const yk_motion_event_t *e = &events[i];
        switch (e->kind) {
        case YK_MOTION_LIFT: out->events |= PP_MO_LIFT; break;
        case YK_MOTION_PUT_DOWN:
            out->events |= PP_MO_PUT;
            if (e->confidence > 0.3f) out->holder = pp_angle_from_motion(e->angle), out->holder_conf = e->confidence;
            break;
        case YK_MOTION_SHAKE_START: out->events |= PP_MO_SHAKE; break;
        case YK_MOTION_FACE_DOWN: out->events |= PP_MO_FACE_DOWN; break;
        case YK_MOTION_FACE_UP: out->events |= PP_MO_FACE_UP; break;
        default: break;
        }
    }
}
