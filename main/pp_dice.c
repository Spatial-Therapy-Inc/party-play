#include "pp_dice.h"

#include "pp_util.h"

#define STEP (1.0f / 240.0f)
#define FRICTION_DOWN 1100.0f
#define FRICTION_HELD 160.0f
#define E_WALL 0.55f
#define E_DICE 0.5f
#define REST_V 40.0f  // a slower touch (no click either) does not bounce: resting on the rim or another die
#define FLIP_SPEED 240.0f
#define FLIP_EVERY 58.0f
#define CALM_V 6.0f
#define CALM_W 0.4f
#define LAND_AFTER 0.28f

static uint8_t other_face(uint32_t *rng, uint8_t face) {
    uint8_t f = (uint8_t)(1 + pp_randi(rng, 5));
    return f >= face ? (uint8_t)(f + 1) : f;  // uniform over the five other faces
}

void pp_dice_init(pp_dice_t *dice, int n, uint32_t seed) {
    dice->rng = seed ? seed : 1;
    dice->n = pp_clampi(n, 1, PP_DICE_MAX);
    for (int i = 0; i < PP_DICE_MAX; i++) {
        pp_die_t *d = &dice->d[i];
        *d = (pp_die_t){0};
        d->face = (uint8_t)(1 + pp_randi(&dice->rng, 6));
        d->a = pp_randr(&dice->rng, -0.4f, 0.4f);
    }
    pp_dice_rest(dice);
}

void pp_dice_rest(pp_dice_t *dice) {
    dice->rolling = false;
    dice->still = 0;
    for (int i = 0; i < dice->n; i++) {
        pp_die_t *d = &dice->d[i];
        const float a = -PP_PI / 2 + i * PP_TAU / dice->n + 0.5f;
        d->x = dice->n == 1 ? 0 : cosf(a) * 62;
        d->y = dice->n == 1 ? 0 : sinf(a) * 62;
        d->vx = d->vy = d->w = d->tum = d->dist = 0;
    }
}

void pp_dice_kick(pp_dice_t *dice) {
    dice->rolling = true;
    dice->still = 0;
    for (int i = 0; i < dice->n; i++) dice->d[i].flips = 0;
}

void pp_dice_throw(pp_dice_t *dice, float x, float y) {
    pp_dice_kick(dice);
    const float base = atan2f(-y, -x);
    for (int i = 0; i < dice->n; i++) {
        pp_die_t *d = &dice->d[i];
        const float a = base + pp_randr(&dice->rng, -0.6f, 0.6f), v = pp_randr(&dice->rng, 700, 1100);
        d->x = pp_clampf(x, -120, 120) * 0.6f + pp_randr(&dice->rng, -30, 30);
        d->y = pp_clampf(y, -120, 120) * 0.6f + pp_randr(&dice->rng, -30, 30);
        d->vx = cosf(a) * v;
        d->vy = sinf(a) * v;
        d->w = pp_randr(&dice->rng, -14, 14);
        d->tum = 1;
    }
    // no overlap to start with
    for (int k = 0; k < 4; k++)
        for (int i = 0; i < dice->n; i++)
            for (int j = i + 1; j < dice->n; j++) {
                pp_die_t *a = &dice->d[i], *b = &dice->d[j];
                float dx = b->x - a->x, dy = b->y - a->y, dist = sqrtf(dx * dx + dy * dy);
                if (dist < 0.01f) dx = 1, dy = 0, dist = 1;
                if (dist < 2 * PP_DIE_R) {
                    const float o = (2 * PP_DIE_R - dist) / 2;
                    dx /= dist, dy /= dist;
                    a->x -= dx * o, a->y -= dy * o, b->x += dx * o, b->y += dy * o;
                }
            }
}

static void substep(pp_dice_t *dice, float h, float fx, float fy, bool held, float jolt, pp_dice_hit_fn hit, void *ctx) {
    const float mu = held ? FRICTION_HELD : FRICTION_DOWN;
    for (int i = 0; i < dice->n; i++) {
        pp_die_t *d = &dice->d[i];
        d->vx += fx * h;
        d->vy += fy * h;
        if (jolt > 0 && pp_randf(&dice->rng) < jolt * 0.2f) {  // knocked off the glass for a moment
            d->vx += pp_randr(&dice->rng, -260, 260);
            d->vy += pp_randr(&dice->rng, -260, 260);
            d->w += pp_randr(&dice->rng, -6, 6);
            d->face = other_face(&dice->rng, d->face);
            if (d->flips < 255) d->flips++;
            d->tum = 1;
        }
        const float sp = sqrtf(d->vx * d->vx + d->vy * d->vy), dec = mu * h;
        if (sp <= dec) {
            d->vx = d->vy = 0;
        } else {
            d->vx *= (sp - dec) / sp;
            d->vy *= (sp - dec) / sp;
        }
        d->w *= expf(-h * (held ? 1.2f : 5.0f));
        d->x += d->vx * h;
        d->y += d->vy * h;
        d->a += d->w * h;
        if (sp > FLIP_SPEED) {
            d->dist += sp * h;
            if (d->dist > FLIP_EVERY) {
                d->dist = 0;
                d->face = other_face(&dice->rng, d->face);
                if (d->flips < 255) d->flips++;
                d->tum = 1;
            }
        }
        const float r = sqrtf(d->x * d->x + d->y * d->y), lim = PP_DICE_ARENA - PP_DIE_R;
        if (r > lim) {
            const float nx = d->x / r, ny = d->y / r, vn = d->vx * nx + d->vy * ny;
            if (vn > 0) {
                // without the rest, dice pushed against the rim by a tilt bounce at a few px/s forever, never calm
                const bool knock = vn > REST_V;
                d->vx -= (1 + (knock ? E_WALL : 0)) * vn * nx;
                d->vy -= (1 + (knock ? E_WALL : 0)) * vn * ny;
                if (knock) d->w += (-d->vx * ny + d->vy * nx) * 0.012f;
                if (vn > 40 && hit) hit(ctx, true, pp_clampf(vn / 1100, 0, 1));
            }
            d->x = nx * lim;
            d->y = ny * lim;
        }
    }
    for (int i = 0; i < dice->n; i++)
        for (int j = i + 1; j < dice->n; j++) {
            pp_die_t *a = &dice->d[i], *b = &dice->d[j];
            float dx = b->x - a->x, dy = b->y - a->y;
            const float dist = sqrtf(dx * dx + dy * dy);
            if (dist >= 2 * PP_DIE_R || dist < 0.01f) continue;
            dx /= dist, dy /= dist;
            const float o = (2 * PP_DIE_R - dist) / 2;
            a->x -= dx * o, a->y -= dy * o, b->x += dx * o, b->y += dy * o;
            const float vrel = (b->vx - a->vx) * dx + (b->vy - a->vy) * dy;
            if (vrel < 0) {
                const bool knock = vrel < -REST_V;  // dice resting on each other neither bounce nor spin
                const float J = -(1 + (knock ? E_DICE : 0)) * vrel / 2;
                a->vx -= J * dx, a->vy -= J * dy, b->vx += J * dx, b->vy += J * dy;
                if (knock) {
                    a->w += pp_randr(&dice->rng, -3, 3);
                    b->w += pp_randr(&dice->rng, -3, 3);
                }
                if (-vrel > 50 && hit) hit(ctx, false, pp_clampf(-vrel / 1000, 0, 1));
            }
        }
    // pushing dice apart can push one past the wall again
    for (int i = 0; i < dice->n; i++) {
        pp_die_t *d = &dice->d[i];
        const float r = sqrtf(d->x * d->x + d->y * d->y), lim = PP_DICE_ARENA - PP_DIE_R;
        if (r > lim) d->x *= lim / r, d->y *= lim / r;
    }
}

bool pp_dice_step(pp_dice_t *dice, float dt, float fx, float fy, bool held, float jolt, pp_dice_hit_fn hit, void *ctx) {
    for (int i = 0; i < dice->n; i++) dice->d[i].tum = fmaxf(0, dice->d[i].tum - dt / 0.14f);
    if (!dice->rolling) return false;
    int n = (int)ceilf(dt / STEP);
    if (n < 1) n = 1;
    if (n > 24) n = 24;  // a long stall (0.1 s) is not caught up in one go
    for (int k = 0; k < n; k++) substep(dice, dt / n, fx, fy, held, k == 0 ? jolt : 0, hit, ctx);
    bool calm = true;
    for (int i = 0; i < dice->n; i++) {
        const pp_die_t *d = &dice->d[i];
        if (sqrtf(d->vx * d->vx + d->vy * d->vy) >= CALM_V || fabsf(d->w) >= CALM_W) calm = false;
    }
    dice->still = (!held && calm) ? dice->still + dt : 0;
    if (dice->still > LAND_AFTER) {
        dice->rolling = false;
        // A new face always differs from the one before, so after only one or two the result would still depend
        // on the last roll (never the same number twice). Those dice draw again from all six as they settle.
        for (int i = 0; i < dice->n; i++)
            if (dice->d[i].flips < 3) {
                dice->d[i].face = (uint8_t)(1 + pp_randi(&dice->rng, 6));
                dice->d[i].tum = 1;
            }
        return true;
    }
    return false;
}

int pp_dice_sum(const pp_dice_t *dice) {
    int s = 0;
    for (int i = 0; i < dice->n; i++) s += dice->d[i].face;
    return s;
}

bool pp_dice_all_same(const pp_dice_t *dice) {
    if (dice->n < 2) return false;
    for (int i = 1; i < dice->n; i++)
        if (dice->d[i].face != dice->d[0].face) return false;
    return true;
}
