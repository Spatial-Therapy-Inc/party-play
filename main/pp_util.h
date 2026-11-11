// Small helpers shared by Party Play's game logic (plain C, host tested; no ESP-IDF).
// Angles are radians in screen coordinates: 0 points right, +pi/2 points down (towards the side key's edge),
// so they grow clockwise on the glass. Positions are pixels from the screen centre (233, 233).
#pragma once

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#define PP_PI 3.14159265358979f
#define PP_TAU 6.28318530717959f
#define PP_W 466
#define PP_C 233

static inline float pp_clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
static inline int pp_clampi(int v, int a, int b) { return v < a ? a : v > b ? b : v; }
static inline float pp_lerpf(float a, float b, float t) { return a + (b - a) * t; }

/** Angle in [-pi, pi). */
static inline float pp_ang_n(float a) {
    a = fmodf(a + PP_PI, PP_TAU);
    if (a < 0) a += PP_TAU;
    return a - PP_PI;
}
/** Angle in [0, 2pi). */
static inline float pp_ang_p(float a) {
    a = fmodf(a, PP_TAU);
    return a < 0 ? a + PP_TAU : a;
}

/** xorshift32: deterministic for tests, seeded from the hardware RNG on the device. Never 0. */
static inline uint32_t pp_rand(uint32_t *s) {
    uint32_t x = *s ? *s : 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x;
}
/** Uniform in [0, 1). */
static inline float pp_randf(uint32_t *s) { return (pp_rand(s) >> 8) * (1.0f / 16777216.0f); }
static inline float pp_randr(uint32_t *s, float a, float b) { return a + (b - a) * pp_randf(s); }
/** Uniform integer in [0, n). */
static inline int pp_randi(uint32_t *s, int n) { return n > 0 ? (int)(pp_randf(s) * n) : 0; }

static inline float pp_ease_out(float t) {
    t = pp_clampf(t, 0, 1);
    return 1 - (1 - t) * (1 - t) * (1 - t);
}
/** Overshoots a little and settles (the "pop" of a die landing). */
static inline float pp_ease_back(float t) {
    t = pp_clampf(t, 0, 1);
    const float c = 1.7f, u = t - 1;
    return 1 + (c + 1) * u * u * u + c * u * u;
}
