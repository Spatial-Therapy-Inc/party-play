// Dice bouncing in the round screen (docs/product/party-play.md "游戏逻辑"): 2D physics at 240 steps a second, a
// new face every 58 px rolled (drawn from the other five, so the final faces are uniform), landing once the device
// is down and every die is still. Plain C, host tested.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define PP_DICE_MAX 3
#define PP_DIE_PX 74        // drawn size
#define PP_DIE_R 38.0f      // collision radius
#define PP_DICE_ARENA 192.0f

typedef struct {
    float x, y, vx, vy;  // from the centre, px and px/s
    float a, w;          // rotation and its speed, rad and rad/s
    float tum;           // 1 right after a new face, falls to 0 in 140 ms (the squash when it flips)
    float dist;          // rolled since the last new face
    uint8_t face;        // 1-6
    uint8_t flips;       // new faces since this roll started (fewer than 3: re-drawn when it lands, see pp_dice.c)
} pp_die_t;

/** What a step heard: collisions, to be turned into sounds. strength is 0..1. */
typedef void (*pp_dice_hit_fn)(void *ctx, bool wall, float strength);

typedef struct {
    pp_die_t d[PP_DICE_MAX];
    int n;
    bool rolling;
    float still;  // seconds every die has been calm while the device is down
    uint32_t rng;
} pp_dice_t;

void pp_dice_init(pp_dice_t *dice, int n, uint32_t seed);
/** Puts the dice at rest around the centre (when the count changes). */
void pp_dice_rest(pp_dice_t *dice);
/** Starts rolling where the dice are (picked up, shaken). */
void pp_dice_kick(pp_dice_t *dice);
/** Thrown from a tap at (x, y) relative to the centre, towards the far side. */
void pp_dice_throw(pp_dice_t *dice, float x, float y);
/** Advances by dt. fx, fy: the push on the dice in the screen plane, px/s^2 (minus the accelerometer's x, y times a
 *  gain: tilting makes them slide downhill, shaking throws them the other way). held: the device is in a hand (low
 *  friction, never lands). jolt: a knock along z this step (0..1), which may flip a die. Returns true on the step
 *  the dice land. */
bool pp_dice_step(pp_dice_t *dice, float dt, float fx, float fy, bool held, float jolt, pp_dice_hit_fn hit, void *ctx);
int pp_dice_sum(const pp_dice_t *dice);
/** All the same face (2 or 3 dice). */
bool pp_dice_all_same(const pp_dice_t *dice);
