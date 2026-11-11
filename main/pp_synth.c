#include "pp_synth.h"

#include <math.h>
#include <string.h>

#include "pp_util.h"

enum { PRIM_TONE, PRIM_NOISE, PRIM_CROWD, PRIM_ROLL, PRIM_CRACKLE };
enum { WAVE_SINE, WAVE_TRI, WAVE_SQUARE, WAVE_SAW };
enum { FILT_BP, FILT_LP, FILT_HP };

// ───────────────────────── building blocks (per-sample work kept to multiplies: no exp/sin per sample)
static float s_sine[257];
static bool s_ready;

static void tables(void) {
    if (s_ready) return;
    for (int i = 0; i <= 256; i++) s_sine[i] = sinf(i * PP_TAU / 256);
    s_ready = true;
}

static inline float osc(int wave, float ph) {
    switch (wave) {
    case WAVE_TRI: return ph < 0.5f ? 4 * ph - 1 : 3 - 4 * ph;
    case WAVE_SQUARE: return ph < 0.5f ? 0.8f : -0.8f;
    case WAVE_SAW: return 2 * ph - 1;
    default: {
        const float x = ph * 256;
        const int i = (int)x;
        return s_sine[i] + (s_sine[i + 1] - s_sine[i]) * (x - i);
    }
    }
}

static inline float noise1(uint32_t *rng) { return (int32_t)pp_rand(rng) * (1.0f / 2147483648.0f); }

/** Topology-preserving state variable filter (stable up to Nyquist). */
typedef struct {
    float a1, a2, a3, k;
} svf_t;

static void svf_set(svf_t *f, float fc, float q, float rate) {
    fc = pp_clampf(fc, 20, rate * 0.45f);
    const float g = tanf(PP_PI * fc / rate);
    f->k = 1.0f / (q > 0.05f ? q : 0.05f);
    f->a1 = 1 / (1 + g * (g + f->k));
    f->a2 = g * f->a1;
    f->a3 = g * f->a2;
}

static inline float svf_run(const svf_t *f, float x, float *ic1, float *ic2, int mode) {
    const float v3 = x - *ic2, v1 = f->a1 * *ic1 + f->a2 * v3, v2 = *ic2 + f->a2 * *ic1 + f->a3 * v3;
    *ic1 = 2 * v1 - *ic1;
    *ic2 = 2 * v2 - *ic2;
    return mode == FILT_LP ? v2 : mode == FILT_BP ? v1 : x - f->k * v1 - v2;
}

static inline float soft(float x) {
    // a gentle limiter: transparent below 0.6, saturating above
    const float ax = fabsf(x);
    if (ax <= 0.6f) return x;
    const float y = (ax - 0.6f) / 0.4f, t = y * (27 + y * y) / (27 + 9 * y * y);
    return (x < 0 ? -1 : 1) * (0.6f + 0.4f * (t > 1 ? 1 : t));
}

// ───────────────────────── primitives
/** Per-sample state of a primitive: envelope and its attack/decay factors, glide factors. Kept beside the primitive
 *  (same index; one synth per firmware), so pp_prim_t stays the description of the sound. ka == 0: not begun. */
typedef struct {
    float env, ka, kd, gk, fcg;
} live_t;

static live_t s_live[PP_SYNTH_PRIMS];
static svf_t s_svf[PP_SYNTH_PRIMS], s_f1[PP_SYNTH_PRIMS], s_f2[PP_SYNTH_PRIMS];

static pp_prim_t *add(pp_synth_t *s) {
    if (s->n >= PP_SYNTH_PRIMS) return NULL;  // full: drop the newest rather than cut a sound off
    s_live[s->n].ka = 0;  // not begun yet
    pp_prim_t *p = &s->p[s->n++];
    memset(p, 0, sizeof(*p));
    p->d = 0.1f;
    p->a = 0.002f;
    p->q = 1;
    return p;
}

static void tone(pp_synth_t *s, float f, float f2, float glide, int wave, float g, float a, float d, float at, float lp) {
    pp_prim_t *p = add(s);
    if (!p) return;
    p->type = PRIM_TONE;
    p->wave = (uint8_t)wave;
    p->f0 = f;
    p->f1 = f2 > 0 ? f2 : f;
    p->glide = glide > 0 ? glide : d;
    p->peak = g;
    p->a = a > 0 ? a : 0.003f;
    p->d = d;
    p->start = at;
    p->lp_fc = lp;
}

static void noise(pp_synth_t *s, int filt, float fc, float fc2, float glide, float q, float g, float a, float d, float at) {
    pp_prim_t *p = add(s);
    if (!p) return;
    p->type = PRIM_NOISE;
    p->filt = (uint8_t)filt;
    p->fc0 = fc;
    p->fc1 = fc2 > 0 ? fc2 : fc;
    p->glide = glide > 0 ? glide : d;
    p->q = q;
    p->peak = g;
    p->a = a > 0 ? a : 0.002f;
    p->d = d;
    p->start = at;
}

static void modal(pp_synth_t *s, float f0, const float (*parts)[3], int n, float g, float at) {
    for (int i = 0; i < n; i++) tone(s, f0 * parts[i][0], 0, 0, WAVE_SINE, g * parts[i][1], 0.002f, parts[i][2], at, 0);
}

static void begin(pp_synth_t *s, pp_prim_t *p, int i) {
    live_t *L = &s_live[i];
    const float r = s->rate, peak = p->peak > 1e-4f ? p->peak : 1e-4f;
    L->env = 1e-4f;
    L->ka = expf(logf(peak / 1e-4f) / (p->a * r));
    L->kd = expf(logf(1e-4f / peak) / (p->d * r));
    L->gk = (p->type == PRIM_TONE && p->f1 != p->f0) ? expf(logf(p->f1 / p->f0) / (p->glide * r)) : 1;
    L->fcg = (p->type == PRIM_NOISE && p->fc1 != p->fc0) ? expf(logf(p->fc1 / p->fc0) / (p->glide * r)) : 1;
    if (p->type == PRIM_NOISE || p->type == PRIM_ROLL || p->type == PRIM_CRACKLE) svf_set(&s_svf[i], p->fc0, p->q, r);
    if (p->type == PRIM_CROWD) {
        svf_set(&s_f1[i], p->form1, 5, r);
        svf_set(&s_f2[i], p->form2, 6, r);
    }
    if (p->lp_fc > 0) p->lp_y = 0;
}

/** One sample of primitive i; returns false when it has finished. */
static bool step(pp_synth_t *s, int i, float *acc) {
    pp_prim_t *p = &s->p[i];
    live_t *L = &s_live[i];
    const float r = s->rate, dt = 1.0f / r;
    if (p->start > 0) {
        p->start -= dt;
        if (p->start <= 0) begin(s, p, i);
        return true;
    }
    float x = 0;
    switch (p->type) {
    case PRIM_TONE: {
        if (p->t < p->a) L->env = fminf(L->env * L->ka, p->peak);
        else L->env *= L->kd;
        if (p->t >= p->a + p->d) return false;
        if (p->t < p->glide) p->f0 *= L->gk;
        p->phase += p->f0 * dt;
        p->phase -= (int)p->phase;
        x = osc(p->wave, p->phase);
        if (p->shape) x = soft(x * 4) * 1.2f;  // the boom's thump: overdriven so its harmonics carry
        if (p->lp_fc > 0) {
            const float al = 1 - expf(-PP_TAU * p->lp_fc / r);
            p->lp_y += al * (x - p->lp_y);
            x = p->lp_y;
        }
        x *= L->env;
        break;
    }
    case PRIM_NOISE: {
        if (p->t < p->a) L->env = fminf(L->env * L->ka, p->peak);
        else L->env *= L->kd;
        if (p->t >= p->a + p->d) return false;
        if (L->fcg != 1 && p->t < p->glide) {
            p->fc0 *= L->fcg;
            if (((int)(p->t * r) & 15) == 0) svf_set(&s_svf[i], p->fc0, p->q, r);
        }
        x = svf_run(&s_svf[i], noise1(&s->rng), &p->s_lp, &p->s_bp, p->filt) * L->env;
        break;
    }
    case PRIM_CROWD: {
        // a "wow": a sawtooth rising and settling, through two vowel formants, linear rise then fading out
        if (p->t >= p->len) return false;
        const float ramp = p->a;
        if (p->t < ramp) L->env = p->peak * p->t / ramp;
        else L->env *= L->kd;
        const float f = p->t < 0.35f ? p->f0 * (1 + 0.35f * p->t / 0.35f)
                                     : p->f0 * (1.35f - 0.25f * (p->t - 0.35f) / (p->len - 0.35f + 1e-3f));
        p->phase += f * dt;
        p->phase -= (int)p->phase;
        const float v = osc(WAVE_SAW, p->phase);
        x = (svf_run(&s_f1[i], v, &p->f1lp, &p->f1bp, FILT_BP) + svf_run(&s_f2[i], v, &p->f2lp, &p->f2bp, FILT_BP)) * L->env;
        break;
    }
    case PRIM_ROLL: {
        // a snare roll: 24 strokes a second, getting louder towards the end
        if (p->t >= p->len) return false;
        const float k = p->t / p->len, per = 1.0f / 24;
        const float into = fmodf(p->t, per);
        if (into < dt) L->env = 0.05f + 0.18f * k;  // a new stroke
        else L->env *= L->kd;
        p->phase += 420 * dt;
        p->phase -= (int)p->phase;
        x = (svf_run(&s_svf[i], noise1(&s->rng), &p->s_lp, &p->s_bp, FILT_BP) + 0.3f * osc(WAVE_SINE, p->phase)) * L->env;
        break;
    }
    case PRIM_CRACKLE: {
        // claps or crackles: short bursts at random times, thinning out (crackle) or steady (claps)
        if (p->t >= p->len) return false;
        const float density = p->f0 * (p->shape ? (1 - p->t / p->len) : 1);  // bursts a second
        if (pp_randf(&s->rng) < density * dt) L->env = p->peak * pp_randr(&s->rng, 0.4f, 1);
        else L->env *= L->kd;
        x = svf_run(&s_svf[i], noise1(&s->rng), &p->s_lp, &p->s_bp, FILT_BP) * L->env;
        break;
    }
    default: return false;
    }
    p->t += dt;
    *acc += x;
    return true;
}

// ───────────────────────── sounds (the prototype's recipes)
static const float DICE_P[3][3] = {{1, 0.35f, 0.05f}, {2.31f, 0.18f, 0.035f}, {3.93f, 0.09f, 0.02f}};
static const float WALL_P[3][3] = {{1, 0.4f, 0.07f}, {2.7f, 0.18f, 0.04f}, {4.2f, 0.08f, 0.03f}};
static const float WHEEL_P[2][3] = {{1, 0.18f, 0.025f}, {2.8f, 0.06f, 0.015f}};
static const float DING_P[3][3] = {{1, 0.3f, 1.0f}, {2.76f, 0.12f, 0.5f}, {5.4f, 0.05f, 0.25f}};
static const float WOOD_P[2][3] = {{1, 0.32f, 0.05f}, {2.6f, 0.1f, 0.03f}};
static const float STOP_P[2][3] = {{1, 0.28f, 0.4f}, {2.4f, 0.08f, 0.2f}};
static const float SEAT_P[2][3] = {{1, 0.3f, 0.35f}, {4, 0.06f, 0.08f}};

static void dice_hit(pp_synth_t *s, float v, bool force) {
    if (!force && s->clock - s->last_hit < 0.018) return;  // rattling dice: not a smear of clicks
    s->last_hit = (float)s->clock;
    const float g = pp_clampf(v, 0.05f, 1);
    noise(s, FILT_BP, pp_lerpf(1800, 4200, g), 0, 0, 1.6f, 0.55f * g, 0, pp_lerpf(0.012f, 0.03f, g), 0);
    modal(s, pp_randr(&s->rng, 1900, 2700), DICE_P, 3, g * 0.6f, 0);
}

static void crowd(pp_synth_t *s, float len) {
    for (int v = 0; v < 6; v++) {
        pp_prim_t *p = add(s);
        if (!p) break;
        p->type = PRIM_CROWD;
        p->f0 = pp_randr(&s->rng, 260, 420);
        p->form1 = pp_randr(&s->rng, 750, 900);
        p->form2 = pp_randr(&s->rng, 1300, 1700);
        p->peak = 0.05f;
        p->a = 0.12f + v * 0.03f;
        p->len = len;
        p->d = len - p->a;
    }
    pp_prim_t *c = add(s);  // claps through it
    if (!c) return;
    c->type = PRIM_CRACKLE;
    c->f0 = 22 / len;
    c->fc0 = 1800;
    c->q = 1.5f;
    c->peak = 0.3f;
    c->d = 0.018f;
    c->len = len;
}

void pp_synth_play(pp_synth_t *s, pp_sound_t id, float g) {
    switch (id) {
    case PP_SND_CONFIRM:
        tone(s, 1568, 0, 0, WAVE_SINE, 0.22f, 0, 0.09f, 0, 0);
        tone(s, 2349, 0, 0, WAVE_SINE, 0.12f, 0, 0.08f, 0.05f, 0);
        break;
    case PP_SND_TICK:
        noise(s, FILT_BP, 3200, 0, 0, 2.5f, 0.16f * g, 0, 0.012f, 0);
        tone(s, 1900, 0, 0, WAVE_SINE, 0.08f * g, 0, 0.018f, 0, 0);
        break;
    case PP_SND_ERROR:
        tone(s, 392, 0, 0, WAVE_SQUARE, 0.12f, 0, 0.12f, 0, 2200);
        tone(s, 330, 0, 0, WAVE_SQUARE, 0.12f, 0, 0.16f, 0.16f, 2200);
        break;
    case PP_SND_DICE_HIT: dice_hit(s, g, false); break;
    case PP_SND_DICE_WALL: {
        if (s->clock - s->last_hit < 0.018) break;
        s->last_hit = (float)s->clock;
        const float v = pp_clampf(g, 0.05f, 1);
        noise(s, FILT_BP, pp_lerpf(900, 2000, v), 0, 0, 1.1f, 0.6f * v, 0, pp_lerpf(0.02f, 0.05f, v), 0);
        modal(s, pp_randr(&s->rng, 620, 820), WALL_P, 3, v * 0.7f, 0);
        break;
    }
    case PP_SND_SETTLE:
        dice_hit(s, 0.35f, true);
        tone(s, 880, 0, 0, WAVE_SINE, 0.06f, 0, 0.06f, 0.06f, 0);
        break;
    case PP_SND_REVEAL:
        tone(s, 660, 1320, 0.08f, WAVE_SINE, 0.22f * (g > 0 ? g : 1), 0, 0.12f, 0, 0);
        tone(s, 1980, 0, 0, WAVE_SINE, 0.08f * (g > 0 ? g : 1), 0, 0.25f, 0.08f, 0);
        break;
    case PP_SND_FANFARE:
    case PP_SND_FANFARE_BIG: {
        static const float N[6] = {523, 659, 784, 1047, 1319, 1568};
        const int n = id == PP_SND_FANFARE_BIG ? 6 : 4;
        for (int i = 0; i < n; i++) {
            tone(s, N[i], 0, 0, WAVE_TRI, 0.22f, 0, 0.26f, i * 0.085f, 0);
            tone(s, N[i] * 2, 0, 0, WAVE_SINE, 0.06f, 0, 0.2f, i * 0.085f, 0);
        }
        tone(s, N[n - 1], 0, 0, WAVE_SQUARE, 0.06f, 0, 0.6f, n * 0.085f, 3000);
        break;
    }
    case PP_SND_CHEER: crowd(s, g > 0.3f ? g : 1.4f); break;
    case PP_SND_WHEEL_TICK:
        noise(s, FILT_BP, 3600, 0, 0, 3, 0.28f * g, 0, 0.008f, 0);
        modal(s, 1250, WHEEL_P, 2, g, 0);
        break;
    case PP_SND_DRUMROLL: {
        pp_prim_t *p = add(s);
        if (!p) break;
        p->type = PRIM_ROLL;
        p->fc0 = 2100;
        p->q = 0.9f;
        p->peak = 0.2f;
        p->d = 0.03f;
        p->len = g > 0.2f ? g : 1.3f;
        break;
    }
    case PP_SND_DING: modal(s, 1318, DING_P, 3, 1, 0); break;
    case PP_SND_BOMB_TICK: modal(s, 940, WOOD_P, 2, g, 0); break;
    case PP_SND_BOMB_TOCK: modal(s, 1180, WOOD_P, 2, g, 0); break;
    case PP_SND_URGE:
        tone(s, 1760, 0, 0, WAVE_SQUARE, 0.08f, 0, 0.05f, 0, 4000);
        tone(s, 1760, 0, 0, WAVE_SQUARE, 0.08f, 0, 0.05f, 0.1f, 4000);
        break;
    case PP_SND_BOOM: {
        noise(s, FILT_LP, 7000, 260, 1.1f, 0.7f, 0.95f, 0.004f, 1.3f, 0);
        tone(s, 140, 60, 0.5f, WAVE_SAW, 0.5f, 0.003f, 0.55f, 0, 1400);
        if (s->n > 0) s->p[s->n - 1].shape = 1;
        pp_prim_t *c = add(s);
        if (c) {
            c->type = PRIM_CRACKLE;
            c->shape = 1;  // thinning out
            c->f0 = 34;
            c->fc0 = 3000;
            c->q = 2;
            c->peak = 0.22f;
            c->d = 0.012f;
            c->len = 1.4f;
            c->start = 0.15f;
        }
        break;
    }
    case PP_SND_FIZZ: noise(s, FILT_HP, 3000, 6000, 0.5f, 0.7f, 0.18f, 0.01f, 0.5f, 0); break;
    case PP_SND_HEART:
        tone(s, 330, 0, 0, WAVE_SINE, 0.18f, 0, 0.09f, 0, 900);
        tone(s, 300, 0, 0, WAVE_SINE, 0.12f, 0, 0.1f, 0.18f, 900);
        break;
    case PP_SND_SNAP_GO:
        noise(s, FILT_BP, 2600, 0, 0, 0.8f, 0.5f, 0, 0.04f, 0);
        tone(s, 1200, 0, 0, WAVE_SQUARE, 0.12f, 0, 0.14f, 0, 5000);
        tone(s, 1800, 0, 0, WAVE_SQUARE, 0.1f, 0, 0.14f, 0, 5000);
        break;
    case PP_SND_DECOY: tone(s, 740, 520, 0.18f, WAVE_TRI, 0.16f, 0, 0.18f, 0, 0); break;
    case PP_SND_TEN_START: tone(s, 1046, 0, 0, WAVE_SINE, 0.22f, 0, 0.1f, 0, 0); break;
    case PP_SND_TEN_TICK: tone(s, 1568, 0, 0, WAVE_SINE, 0.08f, 0, 0.03f, 0, 0); break;
    case PP_SND_TEN_STOP: modal(s, 1760, STOP_P, 2, 1, 0); break;
    case PP_SND_SEAT: {
        static const float F[8] = {523, 587, 659, 784, 880, 1047, 1175, 1319};
        modal(s, F[((int)g) & 7], SEAT_P, 2, 1, 0);
        break;
    }
    case PP_SND_UNSEAT: tone(s, 523, 392, 0.16f, WAVE_SINE, 0.16f, 0, 0.16f, 0, 0); break;
    case PP_SND_OPEN:
        tone(s, 392, 784, 0.22f, WAVE_SINE, 0.16f, 0, 0.25f, 0, 0);
        tone(s, 1175, 0, 0, WAVE_SINE, 0.06f, 0, 0.3f, 0.18f, 0);
        break;
    case PP_SND_CLOSE: tone(s, 784, 392, 0.2f, WAVE_SINE, 0.14f, 0, 0.22f, 0, 0); break;
    case PP_SND_SWISH: noise(s, FILT_BP, 1500, 3500, 0.18f, 1.2f, 0.12f * (g > 0 ? g : 1), 0.03f, 0.18f, 0); break;
    default: break;
    }
}

void pp_synth_init(pp_synth_t *s, int rate, uint32_t seed) {
    tables();
    memset(s, 0, sizeof(*s));
    s->rate = (float)rate;
    s->rng = seed ? seed : 3;
    s->master = 0.9f;
    s->last_hit = -1;
}

void pp_synth_clear(pp_synth_t *s) { s->n = 0; }

void pp_synth_play_at(pp_synth_t *s, pp_sound_t sound, float gain, float delay) {
    const int first = s->n;
    pp_synth_play(s, sound, gain);
    if (delay > 0)
        for (int i = first; i < s->n; i++) s->p[i].start += delay;
}

/** One output sample: every primitive, then the limiter. */
static float sample(pp_synth_t *s) {
    float acc = 0;
    for (int i = 0; i < s->n;) {
        if (step(s, i, &acc)) {
            i++;
            continue;
        }
        // finished: move the last one into its place (with its live state)
        s->n--;
        if (i != s->n) {
            s->p[i] = s->p[s->n];
            s_live[i] = s_live[s->n];
            s_svf[i] = s_svf[s->n];
            s_f1[i] = s_f1[s->n];
            s_f2[i] = s_f2[s->n];
        }
        s_live[s->n].ka = 0;
    }
    s->clock += 1.0 / s->rate;
    return soft(acc * s->master);
}

static void begin_new(pp_synth_t *s) {
    // primitives added since the last render start now (unless they wait)
    for (int i = 0; i < s->n; i++)
        if (s->p[i].t == 0 && s->p[i].start <= 0 && s_live[i].ka == 0) begin(s, &s->p[i], i);
}

int pp_synth_render(pp_synth_t *s, int16_t *out, int n) {
    begin_new(s);
    for (int k = 0; k < n; k++) out[k] = (int16_t)lrintf(sample(s) * 32000);
    return s->n;
}

int pp_synth_render_f(pp_synth_t *s, float *out, int n) {
    begin_new(s);
    for (int k = 0; k < n; k++) out[k] = sample(s) * 0.977f;  // under the mixer's -0.2 dB
    return s->n;
}
