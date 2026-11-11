#include "pp_bomb.h"

#include <string.h>

#include "pp_util.h"

const pp_penalty_t PP_PENALTIES[PP_PENALTY_COUNT] = {
    {"学一声猫叫", "学一声猫叫，要像哦！", "Meow", "Meow like a cat!"},
    {"学小鸡啄米", "学小鸡啄米：用手比划，叫三声", "Chick peck", "Peck like a chick with your hand and cheep three times"},
    {"做个鬼脸", "给大家做个鬼脸", "Funny face", "Make a funny face for everyone"},
    {"头顶画三圈", "双手在头顶画三个圈", "Three circles", "Draw three circles above your head with both hands"},
    {"夸夸右边的人", "夸一夸你右边的人", "Compliment", "Say something nice about the person on your right"},
    {"唱一句歌", "唱一句你喜欢的歌", "Sing a line", "Sing one line of a song you like"},
    {"演一种动物", "演一种动物，让大家猜", "Act an animal", "Act out an animal, everyone guesses"},
    {"机器人说话", "用机器人的声音说：我输了", "Robot voice", "Say \"I lost\" in a robot voice"},
    {"绕口令", "说三遍：吃葡萄不吐葡萄皮", "Tongue twister", "Say \"red lorry, yellow lorry\" three times"},
    {"闭眼摸鼻子", "闭上眼睛摸鼻子", "Nose touch", "Close your eyes and touch your nose"},
    {"慢动作挥手", "用慢动作跟大家挥挥手", "Slow wave", "Wave to everyone in slow motion"},
    {"讲个笑话", "讲一个笑话", "Tell a joke", "Tell a joke"},
    {"拍手十下", "拍手十下，越来越快", "Ten claps", "Clap ten times, faster and faster"},
    {"学 Pip 说话", "学我说：炸啦！", "Be Pip", "Say \"Boom!\" like me"},
    {"假装打哈欠", "假装打一个大哈欠", "Fake yawn", "Fake a big yawn"},
    {"说三种水果", "五秒内说出三种水果", "Three fruits", "Name three fruits in five seconds"},
    {"举手喊耶", "举起双手喊一声耶", "Hands up", "Raise both hands and shout \"yay!\""},
    {"倒数十个数", "从十倒数到一，越来越慢", "Count down", "Count down from ten, slower and slower"},
    {"摆个造型", "摆一个最酷的造型", "Cool pose", "Strike your coolest pose"},
    {"下局你先拿", "下一局你先拿炸弹", "You start", "You start the next round with the bomb"},
};

void pp_bomb_init(pp_bomb_t *b, uint32_t seed) {
    memset(b, 0, sizeof(*b));
    b->rng = seed ? seed : 13;
    b->penalty = -1;
    b->st = PP_BOMB_IDLE;
}

int pp_bomb_next_penalty(pp_bomb_t *b) {
    if (b->bag_n == 0) {
        for (int i = 0; i < PP_PENALTY_COUNT; i++) b->bag[i] = (uint8_t)i;
        for (int i = PP_PENALTY_COUNT - 1; i > 0; i--) {  // Fisher-Yates
            const int j = pp_randi(&b->rng, i + 1);
            const uint8_t t = b->bag[i];
            b->bag[i] = b->bag[j];
            b->bag[j] = t;
        }
        b->bag_n = PP_PENALTY_COUNT;
    }
    return b->bag[--b->bag_n];
}

// When it goes off: no sooner than MIN s (whoever lit it can pass it once), then about 1 in MEAN every second, never
// after MAX. The ticking reaches its fastest at RAMP s whenever that is.
#define BOMB_MIN 2.0f
#define BOMB_MEAN 22.0f
#define BOMB_MAX 50.0f
#define BOMB_RAMP 25.0f
#define QUICK_MIN 1.0f
#define QUICK_MEAN 5.0f
#define QUICK_MAX 13.0f
#define QUICK_RAMP 8.0f

/** min + an exponential of the given mean, cut off at max: the same chance every second, and nothing piled up at max
 *  (the inverse of the truncated distribution, not a clamp). */
static float hidden_length(uint32_t *rng, float min, float mean, float max) {
    const float u = pp_randf(rng), k = 1 - expf(-(max - min) / mean);
    return pp_clampf(min - mean * logf(1 - u * k), min, max);
}

void pp_bomb_tap(pp_bomb_t *b, bool quick) {
    if (b->st == PP_BOMB_IDLE) {
        b->st = PP_BOMB_LIT;
        b->p = b->ramp = 0;
        b->T = quick ? hidden_length(&b->rng, QUICK_MIN, QUICK_MEAN, QUICK_MAX)
                     : hidden_length(&b->rng, BOMB_MIN, BOMB_MEAN, BOMB_MAX);
        b->quick = quick;
        b->t = b->tick = b->still = b->beep = 0;
        b->nag_cool = 0;
        b->urging = false;
    } else if (b->st == PP_BOMB_PENALTY || b->st == PP_BOMB_AFTER) {
        b->st = PP_BOMB_IDLE;
        b->t = 0;
        b->penalty = -1;
    }
}

bool pp_bomb_cancel(pp_bomb_t *b) {
    if (b->st == PP_BOMB_LIT) {
        b->st = PP_BOMB_IDLE;
        b->t = 0;
        return true;
    }
    if (b->st == PP_BOMB_PENALTY) {
        b->st = PP_BOMB_AFTER;
        b->t = 0;
        return true;
    }
    return false;
}

int pp_bomb_update(pp_bomb_t *b, float dt, bool moving, bool penalties) {
    b->t += dt;
    int ev = 0;
    switch (b->st) {
    case PP_BOMB_LIT: {
        b->still = moving ? 0 : b->still + dt;
        b->urging = b->still > 3;
        const float faster = b->urging ? 1.6f : 1.0f;
        b->p += dt / b->T * faster;
        b->ramp = fminf(b->ramp + dt / (b->quick ? QUICK_RAMP : BOMB_RAMP) * faster, 1);
        const float iv = pp_lerpf(0.62f, 0.12f, powf(b->ramp, 1.4f)) / (b->urging ? 1.25f : 1.0f);
        b->tick += dt;
        if (b->tick >= iv) {
            b->tick = 0;
            b->alt = !b->alt;
            ev |= PP_BOMB_EV_TICK;
        }
        b->nag_cool -= dt;
        if (b->urging) {
            b->beep += dt;
            if (b->beep >= 1.2f) {
                b->beep = 0;
                ev |= PP_BOMB_EV_BEEP;
            }
            if (b->nag_cool <= 0) {
                b->nag_cool = 5;
                ev |= PP_BOMB_EV_NAG;
            }
        } else {
            b->beep = 0;
        }
        if (b->p >= 1) {
            b->st = PP_BOMB_BOOM;
            b->t = 0;
            ev |= PP_BOMB_EV_BOOM;
        }
        break;
    }
    case PP_BOMB_BOOM:
        // Yay's big reaction first (about 2.8 s with a forfeit to follow, 1.3 s without)
        if (b->t > (penalties ? 2.8f : 1.3f)) {
            b->t = 0;
            if (penalties) {
                b->penalty = pp_bomb_next_penalty(b);
                b->st = PP_BOMB_PENALTY;
                ev |= PP_BOMB_EV_PENALTY;
            } else {
                b->st = PP_BOMB_AFTER;
            }
        }
        break;
    case PP_BOMB_PENALTY:
        if (b->t > 8) {
            b->st = PP_BOMB_AFTER;
            b->t = 0;
        }
        break;
    default:
        break;
    }
    return ev;
}
