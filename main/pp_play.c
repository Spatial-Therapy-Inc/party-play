#include "pp_play.h"

#include <math.h>
#include <string.h>

#include "pp_gesture.h"
#include "pp_util.h"

#define COLOR_CORAL 0xDF795D
#define COLOR_AMBER 0xE3A857
#define COLOR_CREAM 0xF3E6C8
#define COLOR_SAGE 0x9FB38C
#define COLOR_GREY 0xA5AA9E
#define COLOR_DEBRIS 0x5A625B

#define MENU_GAMES 5
#define MENU_STEP (PP_TAU / MENU_GAMES)
#define IDLE_DIM 60.0f
#define IDLE_REST_S 3.0f  // Yay rests this long before the screen goes off
#define ROUND_QUIET_S 300.0f  // a round nobody has touched or moved for this long was left: it sleeps after all

static const pp_scene_t MENU_SCENES[MENU_GAMES] = {PP_SC_DICE, PP_SC_WHEEL, PP_SC_REACT, PP_SC_BOMB, PP_SC_TEN};

static const char *const LINES[PP_LINE_COUNT][2] = {
    [PP_LINE_HELLO] = {"今晚玩什么？", "What shall we play tonight?"},
    [PP_LINE_DICE_GO] = {"拿起来摇！", "Pick me up and shake!"},
    [PP_LINE_SUM] = {"点！", "!"},
    [PP_LINE_PAIR] = {"对子！", "Doubles!"},
    [PP_LINE_TRIPLE] = {"豹子！", "Triples!"},
    [PP_LINE_WHEEL_GO] = {"转一下，看看是谁", "Spin it, let's see who"},
    [PP_LINE_ITS_YOU] = {"就是你！", "It's you!"},
    [PP_LINE_SEATS] = {"围一圈，点你面前的边", "Sit around and tap the edge in front of you"},
    [PP_LINE_SNAP_GO] = {"亮了就拍你那块！", "When it lights up, slap your side!"},
    [PP_LINE_TOO_EARLY] = {"抢跑了！", "Too early!"},
    [PP_LINE_FAST] = {"真快！", "So fast!"},
    [PP_LINE_SNAP_WIN] = {"赢啦！", "Winner!"},
    [PP_LINE_TIE] = {"同时！再来", "A tie! Again"},
    [PP_LINE_NOBODY] = {"没人拍？再来", "Nobody? Again"},
    [PP_LINE_BOMB_GO] = {"点着了，快传！", "It's lit, pass it on!"},
    [PP_LINE_PASS_IT] = {"快传！", "Pass it!"},
    [PP_LINE_BOOM] = {"炸啦！", "Boom!"},
    [PP_LINE_PENALTY] = {"小惩罚", "Forfeit"},
    [PP_LINE_TEN_GO] = {"凭感觉，停在十秒", "Stop at ten, by feel"},
    [PP_LINE_PERFECT] = {"完美！", "Perfect!"},
    [PP_LINE_CLOSE] = {"很准！", "Very close!"},
    [PP_LINE_OK] = {"还不错", "Not bad"},
    [PP_LINE_AGAIN] = {"再来一次？", "One more go?"},
    [PP_LINE_BEST] = {"最准的是你", "You're the closest"},
    [PP_LINE_LOW_BAT] = {"我快没电了", "I'm running low"},
    [PP_LINE_PLUG_IN] = {"插上电再玩吧", "Plug me in to keep playing"},
};

const char *pp_line_text(pp_line_t line, bool en) { return line < PP_LINE_COUNT ? LINES[line][en ? 1 : 0] : ""; }

const char *pp_scene_name(pp_scene_t s) {
    static const char *const N[PP_SC_COUNT] = {"menu", "seats", "dice", "wheel", "react", "bomb", "ten", "settings", "scores", "charge"};
    return s < PP_SC_COUNT ? N[s] : "?";
}

// ───────────────────────── outputs
static void out(pp_play_t *p, pp_out_kind_t kind, int id, float gain) {
    if (p->nout >= (int)(sizeof(p->out) / sizeof(p->out[0]))) return;  // the device drains every frame
    p->out[p->nout++] = (pp_out_t){(uint8_t)kind, (uint8_t)id, gain};
}
static void snd(pp_play_t *p, pp_sound_t s, float gain) { out(p, PP_OUT_SOUND, s, gain); }
static void say(pp_play_t *p, pp_line_t l, float num) { out(p, PP_OUT_LINE, l, num); }

int pp_play_take(pp_play_t *p, pp_out_t *o, int max) {
    const int n = p->nout < max ? p->nout : max;
    memcpy(o, p->out, sizeof(pp_out_t) * n);
    memmove(p->out, p->out + n, sizeof(pp_out_t) * (p->nout - n));
    p->nout -= n;
    return n;
}

static void result_changed(pp_play_t *p) {
    p->session.players = p->seats.n;
    p->result_dirty = true;
    out(p, PP_OUT_RESULT, 0, 0);
}

// ───────────────────────── facing, Yay, particles
#define TURN_OUT_S 0.12f     // a turn's dark half
#define TURN_IN_S 0.15f      // and its light half
#define LIGHT_STEP_S 0.034f  // the most the light moves in one tick: a frame at 30 fps

static float turn_light(float t) { return t < TURN_OUT_S ? 1 - t / TURN_OUT_S : fminf(1, (t - TURN_OUT_S) / TURN_IN_S); }

/** The middle faces rot_target at once; a turn already going dark comes back up from the light it has (no jump). */
static void turn_now(pp_play_t *p) {
    if (p->turn_t >= 0) {
        const float k = turn_light(p->turn_t);
        p->turn_t = k >= 1 ? -1 : TURN_OUT_S + k * TURN_IN_S;
    }
    p->rot = p->rot_target;
}

/** Turns the middle towards whoever is at angle phi (snap: to the nearest quarter). A touch turns it through dark
 *  (pp_play_light); cut turns it at once, for a tick whose screen is drawn whole anyway (a result, a new state), where
 *  going dark would only blink. A scene not yet drawn is dark already: at once too. */
static void face_to(pp_play_t *p, float phi, bool snap, bool cut) {
    float r = phi - PP_PI / 2;
    if (snap) r = roundf(r / (PP_PI / 2)) * (PP_PI / 2);
    p->rot_target = p->rot + pp_ang_n(r - p->rot);
    if (cut || p->fade < 0) turn_now(p);
}
static void face(pp_play_t *p, float phi, bool snap) { face_to(p, phi, snap, false); }
static void face_cut(pp_play_t *p, float phi) { face_to(p, phi, false, true); }

static void yay(pp_play_t *p, pp_yay_mode_t mode, pp_pose_t pose, float dur, float ox, float oy, float rot) {
    p->yay = (pp_yay_t){mode, pose, dur > 0 ? p->now + dur : INFINITY, ox, oy, rot, p->yay.serial + 1};
}
static void yay_here(pp_play_t *p, pp_yay_mode_t mode, pp_pose_t pose, float dur, float ox, float oy) {
    yay(p, mode, pose, dur, ox, oy, p->rot_target);
}
static void yay_hide(pp_play_t *p) {
    if (p->yay.mode == PP_YAY_HIDDEN) return;
    p->yay.mode = PP_YAY_HIDDEN;
    p->yay.serial++;
}

static void burst(pp_play_t *p, float x, float y, int n, const uint32_t *colors, int ncolors, float speed,
                  pp_part_kind_t kind, float dir, float spread) {
    for (int i = 0; i < n && p->nparts < PP_PARTS_MAX; i++) {
        const float a = spread >= PP_TAU ? pp_randr(&p->rng, 0, PP_TAU) : dir + pp_randr(&p->rng, -spread / 2, spread / 2);
        const float v = pp_randr(&p->rng, speed * 0.4f, speed);
        p->parts[p->nparts++] = (pp_part_t){
            .x = x, .y = y, .vx = cosf(a) * v, .vy = sinf(a) * v,
            .r = pp_randr(&p->rng, 0, PP_TAU), .vr = pp_randr(&p->rng, -9, 9),
            .life = pp_randr(&p->rng, 0.9f, 1.5f), .s = pp_randr(&p->rng, 0.8f, 1.3f),
            .color = colors[i % ncolors], .kind = (uint8_t)kind,
        };
    }
}

static void parts_update(pp_play_t *p, float dt) {
    for (int i = p->nparts - 1; i >= 0; i--) {
        pp_part_t *q = &p->parts[i];
        const float drag = q->kind == PP_PART_SMOKE ? 1.6f : 2.4f;
        q->vx *= expf(-dt * drag);
        q->vy *= expf(-dt * drag);
        q->x += q->vx * dt;
        q->y += q->vy * dt;
        q->r += q->vr * dt;
        q->life -= dt;
        if (q->life <= 0) p->parts[i] = p->parts[--p->nparts];
    }
}

static float seat_x(const pp_play_t *p, int i, float r) { return PP_C + r * cosf(p->seats.s[i].a); }
static float seat_y(const pp_play_t *p, int i, float r) { return PP_C + r * sinf(p->seats.s[i].a); }

// ───────────────────────── scenes
static void enter(pp_play_t *p, pp_scene_t s);

void pp_play_go(pp_play_t *p, pp_scene_t s) {
    yay_hide(p);
    p->sheet = false;
    p->scene = s;
    p->scene_t = 0;
    p->fade = -1;  // dark until its first frame is drawn
    enter(p, s);
    out(p, PP_OUT_SCENE, s, 0);
}

static void ensure_seats(pp_play_t *p) {
    if (p->seats.n < 2) pp_seats_even(&p->seats, p->players);
}

static void start_game(pp_play_t *p, pp_scene_t s) {
    if ((s == PP_SC_WHEEL || s == PP_SC_REACT) && p->seats.n < 2) {
        p->seats_next = s;
        pp_play_go(p, PP_SC_SEATS);
    } else {
        pp_play_go(p, s);
    }
}

static void enter(pp_play_t *p, pp_scene_t s) {
    switch (s) {
    case PP_SC_MENU:
        p->menu_vel = 0;
        p->menu_snapping = false;
        p->menu_held = false;
        if (p->first_menu) {
            p->first_menu = false;
            yay_here(p, PP_YAY_HOST, PP_POSE_GREET, 1.9f, 0, 0);
            say(p, PP_LINE_HELLO, 0);
        }
        break;
    case PP_SC_SEATS:
        p->seating.n = 0;
        p->color_at = -1;
        say(p, PP_LINE_SEATS, 0);
        break;
    case PP_SC_DICE:
        if (p->dice.n != p->dice_n) pp_dice_init(&p->dice, p->dice_n, pp_rand(&p->rng));
        else pp_dice_rest(&p->dice);
        p->dice_st = PP_DICE_IDLE;
        p->rim_acc = 0;
        for (int i = 0; i < PP_DICE_MAX; i++) p->scale[i] = 1;
        yay_here(p, PP_YAY_PEEK, PP_POSE_GREET, 1.6f, 0, 165);
        say(p, PP_LINE_DICE_GO, 0);
        break;
    case PP_SC_WHEEL:
        ensure_seats(p);
        pp_wheel_init(&p->wheel, &p->seats, pp_rand(&p->rng));
        p->wheel_st = PP_WHEEL_IDLE;
        memset(p->flash, 0, sizeof(p->flash));
        say(p, PP_LINE_WHEEL_GO, 0);
        break;
    case PP_SC_REACT:
        ensure_seats(p);
        pp_react_init(&p->react, p->seats.n, pp_rand(&p->rng));
        p->go_shown = 0;
        yay_here(p, PP_YAY_HOST, PP_POSE_GREET, 1.7f, 0, 0);
        say(p, PP_LINE_SNAP_GO, 0);
        break;
    case PP_SC_BOMB:
        if (p->bomb.st == PP_BOMB_LIT || p->bomb.st == PP_BOMB_BOOM) p->bomb.st = PP_BOMB_IDLE;
        if (p->bomb.rng == 0) pp_bomb_init(&p->bomb, pp_rand(&p->rng));
        p->bomb.st = PP_BOMB_IDLE;
        p->bomb.t = 0;
        break;
    case PP_SC_TEN:
        pp_ten_init(&p->ten, p->seats.n >= 2 ? p->seats.n : 1);
        if (p->ten.multi) face(p, p->seats.s[0].a, false);
        say(p, PP_LINE_TEN_GO, 0);
        break;
    case PP_SC_SETTINGS:
        p->set_acc = 0;
        break;
    case PP_SC_CHARGE:
        yay_here(p, PP_YAY_HOST, PP_POSE_RESTING, 0, 0, 0);
        say(p, PP_LINE_PLUG_IN, 0);
        break;
    default:
        break;
    }
}

void pp_play_init(pp_play_t *p, uint32_t seed) {
    memset(p, 0, sizeof(*p));
    p->rng = seed ? seed : 0x1234567u;
    p->players = 4;
    p->dice_n = 2;
    p->volume = 7;
    p->penalties = true;
    p->first_menu = true;
    p->battery_pct = -1;
    p->turn_t = -1;
    p->idle_off = 30;  // the shared default (BOARD_PREF_SCREEN_OFF_DEFAULT, 30 s since P22); main.c reads the device's
    p->menu_last = 0;
    p->back = PP_SC_MENU;
    pp_dice_init(&p->dice, p->dice_n, pp_rand(&p->rng));
    pp_bomb_init(&p->bomb, pp_rand(&p->rng));
    pp_session_reset(&p->session);
    p->yay.until = INFINITY;
    pp_play_go(p, PP_SC_MENU);
}

void pp_play_launch(pp_play_t *p, const pp_launch_params_t *lp) {
    if (lp->players) {
        p->players = lp->players;
        pp_seats_even(&p->seats, lp->players);
        memset(p->wins, 0, sizeof(p->wins));
    }
    if (lp->dice) p->dice_n = lp->dice;
    if (lp->game >= 0 && lp->game < PP_GAME_COUNT) {
        p->first_menu = false;
        p->menu_pos = (float)lp->game;
        p->menu_last = lp->game;
        start_game(p, MENU_SCENES[lp->game]);
    }
}

// ───────────────────────── menu
static int menu_sel(const pp_play_t *p) {
    int s = (int)lroundf(p->menu_pos) % MENU_GAMES;
    return s < 0 ? s + MENU_GAMES : s;
}

static void menu_tap(pp_play_t *p, float x, float y) {
    const float r = pp_point_radius(x, y);
    if (r < 128) {
        snd(p, PP_SND_OPEN, 1);
        start_game(p, MENU_SCENES[menu_sel(p)]);
        return;
    }
    if (r < 140) return;
    face(p, pp_point_angle(x, y), true);
    for (int i = 0; i < MENU_GAMES; i++) {
        const float a = p->rot_target - PP_PI / 2 + (i - p->menu_pos) * MENU_STEP;
        const float ix = PP_C + 178 * cosf(a), iy = PP_C + 178 * sinf(a);
        if (hypotf(x - ix, y - iy) < 44) {
            float d = fmodf(i - p->menu_pos, MENU_GAMES);
            if (d > 2.5f) d -= MENU_GAMES;
            if (d < -2.5f) d += MENU_GAMES;
            p->menu_snap = p->menu_pos + d;
            p->menu_snapping = true;
            p->menu_vel = 0;
            snd(p, PP_SND_CONFIRM, 1);
            return;
        }
    }
}

static void menu_update(pp_play_t *p, float dt) {
    if (!p->menu_held) {
        if (p->menu_snapping) {
            p->menu_pos += (p->menu_snap - p->menu_pos) * (1 - expf(-dt * 12));
            if (fabsf(p->menu_snap - p->menu_pos) < 0.002f) p->menu_pos = p->menu_snap, p->menu_snapping = false;
        } else if (fabsf(p->menu_vel) > 0.6f) {
            p->menu_pos += p->menu_vel * dt;
            p->menu_vel *= expf(-dt * 2.6f);
        } else {
            p->menu_vel = 0;
            const float t = roundf(p->menu_pos);
            p->menu_pos += (t - p->menu_pos) * (1 - expf(-dt * 14));
        }
    }
    const int s = (int)lroundf(p->menu_pos);
    if (s != p->menu_last) {
        p->menu_last = s;
        snd(p, PP_SND_TICK, 0.8f);
    }
}

// ───────────────────────── seating
static void seats_tap(pp_play_t *p, float x, float y) {
    if (p->color_at >= 0) return;
    const float r = pp_point_radius(x, y), a = pp_point_angle(x, y);
    if (r >= 132) {
        const int res = pp_seats_toggle(&p->seating, a);
        if (res > 0) {
            p->seat_born[p->seating.n - 1] = p->now;
            snd(p, PP_SND_SEAT, (float)(p->seating.n - 1));
            face(p, a, true);
        } else if (res < 0) {
            snd(p, PP_SND_UNSEAT, 1);
        } else {
            snd(p, PP_SND_ERROR, 1);
        }
    } else if (r < 96) {
        if (p->seating.n == 0) {
            pp_seats_even(&p->seats, p->players);
            memset(p->wins, 0, sizeof(p->wins));
            snd(p, PP_SND_CONFIRM, 1);
            pp_play_go(p, p->seats_next);
        } else if (p->seating.n == 1) {
            snd(p, PP_SND_ERROR, 1);
        } else {
            p->seats = p->seating;
            p->seats.tapped = true;
            pp_seats_sort_color(&p->seats);
            p->players = p->seats.n;
            memset(p->wins, 0, sizeof(p->wins));
            p->color_at = 0;
            p->color_t = 0;
        }
    }
}

static void seats_update(pp_play_t *p, float dt) {
    if (p->color_at < 0) return;
    p->color_t += dt;
    const int k = (int)(p->color_t / 0.11f);
    if (k > p->color_at && p->color_at < p->seats.n) {
        snd(p, PP_SND_SEAT, (float)p->color_at);
        p->color_at++;
    }
    if (p->color_t > p->seats.n * 0.11f + 0.45f) pp_play_go(p, p->seats_next);
}

// ───────────────────────── dice
static void dice_hit(void *ctx, bool wall, float strength) {
    pp_play_t *p = ctx;
    snd(p, wall ? PP_SND_DICE_WALL : PP_SND_DICE_HIT, strength);
}

static void dice_start(pp_play_t *p) {
    for (int i = 0; i < PP_DICE_MAX; i++) p->scale[i] = 1;
    p->dice_special = false;
    p->dice_st = PP_DICE_ROLL;
}

static void dice_land(pp_play_t *p) {
    p->dice_st = PP_DICE_POP;
    p->reveal_t = 0;
    snd(p, PP_SND_SETTLE, 1);
    face_cut(p, p->roller);
    for (int i = 0; i < p->dice.n; i++) {
        p->from[i][0] = p->dice.d[i].x;
        p->from[i][1] = p->dice.d[i].y;
        p->from[i][2] = p->rot_target + pp_ang_n(p->dice.d[i].a - p->rot_target);
    }
}

static void dice_result(pp_play_t *p) {
    p->dice_st = PP_DICE_RESULT;
    const int sum = pp_dice_sum(&p->dice), n = p->dice.n;
    const bool same = pp_dice_all_same(&p->dice);
    p->dice_special = same;
    p->rolls++;
    pp_session_round(&p->session, PP_GAME_DICE);
    pp_session_dice(&p->session, n, same ? p->dice.d[0].face : 0);
    result_changed(p);
    static const uint32_t C3[] = {COLOR_CORAL, COLOR_AMBER, COLOR_CREAM, COLOR_SAGE};
    if (same && n == 3) {
        snd(p, PP_SND_FANFARE_BIG, 1);
        snd(p, PP_SND_CHEER, 1.8f);
        burst(p, PP_C, PP_C, 36, C3, 4, 420, PP_PART_CONFETTI, 0, PP_TAU);
        yay_here(p, PP_YAY_PEEK, PP_POSE_DONE, 2, 0, 165);
        say(p, PP_LINE_TRIPLE, 0);
    } else if (same) {
        snd(p, PP_SND_FANFARE, 1);
        snd(p, PP_SND_CHEER, 1.1f);
        burst(p, PP_C, PP_C, 26, C3, 3, 360, PP_PART_CONFETTI, 0, PP_TAU);
        yay_here(p, PP_YAY_PEEK, PP_POSE_DONE, 1.8f, 0, 165);
        say(p, PP_LINE_PAIR, 0);
    } else {
        say(p, PP_LINE_SUM, (float)sum);
    }
}

static void dice_tap(pp_play_t *p, float x, float y) {
    if (p->dice_st == PP_DICE_ROLL || p->dice_st == PP_DICE_POP || p->dice_st == PP_DICE_ARRANGE) return;
    p->roller = pp_point_angle(x, y);
    dice_start(p);
    pp_dice_throw(&p->dice, x - PP_C, y - PP_C);
    snd(p, PP_SND_SWISH, 1);
}

static void dice_update(pp_play_t *p, float dt, const pp_motion_in_t *mi) {
    if (mi->events & (PP_MO_LIFT | PP_MO_SHAKE)) {
        if (mi->events & PP_MO_LIFT) p->roller = mi->holder;
        if (p->dice_st == PP_DICE_IDLE || p->dice_st == PP_DICE_RESULT) {
            dice_start(p);
            pp_dice_kick(&p->dice);
        }
    }
    if (p->dice_st == PP_DICE_ROLL) {
        if (mi->held && mi->holder_conf > 0.3f) p->roller = mi->holder;
        if (pp_dice_step(&p->dice, dt, mi->fx, mi->fy, mi->held, mi->jolt, dice_hit, p)) dice_land(p);
    } else {
        pp_dice_step(&p->dice, dt, 0, 0, false, 0, NULL, NULL);  // the flip squash winds down
    }
    if (p->dice_st == PP_DICE_POP || p->dice_st == PP_DICE_ARRANGE) {
        p->reveal_t += dt;
        const int n = p->dice.n;
        static const float T1[1][2] = {{0, -6}}, T2[2][2] = {{-60, -46}, {60, -46}}, T3[3][2] = {{-100, -46}, {0, -46}, {100, -46}};
        const float(*target)[2] = n == 1 ? T1 : n == 2 ? T2 : T3;
        const float scale = n == 1 ? 2.0f : n == 2 ? 1.3f : 1.12f;
        if (p->reveal_t < 0.32f) {
            const float k = pp_ease_back(p->reveal_t / 0.32f);
            for (int i = 0; i < n; i++) p->scale[i] = pp_lerpf(1, 1.22f, k);
            if (p->reveal_t + dt >= 0.28f && p->reveal_t < 0.28f) snd(p, PP_SND_REVEAL, 1);
        } else {
            p->dice_st = PP_DICE_ARRANGE;
            const float k = pp_ease_out((p->reveal_t - 0.32f) / 0.55f), c = cosf(p->rot), s = sinf(p->rot);
            for (int i = 0; i < n; i++) {
                const float X = target[i][0] * c - target[i][1] * s, Y = target[i][0] * s + target[i][1] * c;
                p->dice.d[i].x = pp_lerpf(p->from[i][0], X, k);
                p->dice.d[i].y = pp_lerpf(p->from[i][1], Y, k);
                p->dice.d[i].a = pp_lerpf(p->from[i][2], p->rot, k);
                p->scale[i] = pp_lerpf(1.22f, scale, k);
            }
            if (p->reveal_t > 0.9f) dice_result(p);
        }
    }
}

static void dice_rim(pp_play_t *p, float da) {
    if (p->dice_st != PP_DICE_IDLE && p->dice_st != PP_DICE_RESULT) return;
    p->rim_acc += da;
    if (fabsf(p->rim_acc) <= 0.7f) return;
    const int d = p->rim_acc > 0 ? 1 : -1;
    p->rim_acc = 0;
    const int n = pp_clampi(p->dice_n + d, 1, PP_DICE_MAX);
    if (n == p->dice_n) {
        snd(p, PP_SND_TICK, 0.4f);
        return;
    }
    p->dice_n = n;
    pp_dice_init(&p->dice, n, pp_rand(&p->rng));
    p->dice_st = PP_DICE_IDLE;
    for (int i = 0; i < PP_DICE_MAX; i++) p->scale[i] = 1;
    snd(p, PP_SND_TICK, 1);
    snd(p, PP_SND_REVEAL, 0.6f);
}

// ───────────────────────── wheel
static void wheel_spin(pp_play_t *p, float w) {
    pp_wheel_spin(&p->wheel, &p->seats, w);
    p->wheel_st = PP_WHEEL_SPINNING;
    p->spins++;
    snd(p, PP_SND_SWISH, 1);
}

static void wheel_update(pp_play_t *p, float dt) {
    for (int i = 0; i < PP_SEATS_MAX; i++) p->flash[i] = fmaxf(0, p->flash[i] - dt * 3);
    if (p->wheel_st != PP_WHEEL_SPINNING) return;
    const int ev = pp_wheel_update(&p->wheel, &p->seats, dt);
    if (ev & PP_WHEEL_TICK) {
        snd(p, PP_SND_WHEEL_TICK, p->wheel.tick_gain);
        if (pp_wheel_speed(&p->wheel) < 8) p->flash[p->wheel.under] = 1;  // too fast to see: no flash
    }
    if (ev & PP_WHEEL_DRUM) snd(p, PP_SND_DRUMROLL, p->wheel.drum_s);
    if (ev & PP_WHEEL_DONE) {
        p->wheel_st = PP_WHEEL_RESULT;
        p->result_at = p->now;
        snd(p, PP_SND_DING, 1);
        snd(p, PP_SND_CHEER, 1.2f);
        const int w = p->wheel.winner;
        face_cut(p, p->seats.s[w].a);
        pp_session_round(&p->session, PP_GAME_WHEEL);
        result_changed(p);
        const uint32_t cols[] = {PP_SEAT_COLORS[p->seats.s[w].color], COLOR_CREAM, COLOR_AMBER};
        burst(p, seat_x(p, w, 168), seat_y(p, w, 168), 30, cols, 3, 360, PP_PART_CONFETTI, p->seats.s[w].a + PP_PI, 2.2f);
        yay_here(p, PP_YAY_PEEK, PP_POSE_DONE, 1.8f, 0, -116);
        say(p, PP_LINE_ITS_YOU, 0);
    }
}

// ───────────────────────── react
static void react_judge(pp_play_t *p, bool now);

static void react_tap(pp_play_t *p, float x, float y, float at) {
    pp_react_t *r = &p->react;
    if (r->st == PP_RX_READY) {
        if (pp_point_radius(x, y) < 90) {
            snd(p, PP_SND_CONFIRM, 1);
            pp_react_start(r);
        }
        return;
    }
    if (r->st == PP_RX_OVER) {
        pp_react_start(r);
        snd(p, PP_SND_CONFIRM, 1);
        return;
    }
    if (pp_point_radius(x, y) < 96) return;  // dead zone: two people at once can report the point between them
    const int seat = pp_seats_nearest(&p->seats, pp_point_angle(x, y));
    if (r->st == PP_RX_GO) {  // judged in react_judge(): someone whose part lit later may still be faster
        if (r->locked[seat]) return;
        for (int i = 0; i < p->nslaps; i++)
            if (p->slaps[i].seat == seat) return;
        if (p->nslaps < PP_SEATS_MAX) p->slaps[p->nslaps].seat = seat, p->slaps[p->nslaps++].at = at;
        react_judge(p, false);
        return;
    }
    const int ev = pp_react_slap(r, seat, 0);
    if (ev & PP_RX_EV_FALSE) {
        snd(p, PP_SND_ERROR, 1);
        face_cut(p, p->seats.s[seat].a);  // waiting for the lit frame: never dark
        yay(p, PP_YAY_PEEK, PP_POSE_HELP, 1.1f, 0, -116, pp_ang_n(p->seats.s[seat].a - PP_PI / 2));
        say(p, PP_LINE_TOO_EARLY, 0);
    }
}

#define TOUCH_LATE_S 0.01f  // a touch can reach us this much after a later one (the controller's report)

/** When seat k's reaction counts from: the whole lit frame on the glass, or its own part of it (react_by_part). */
static float react_start(const pp_play_t *p, int k, bool by_part) { return by_part ? p->seen[k] : p->go_shown; }

/** The fastest slap on the lit frame wins. It waits until no seat that has not slapped could still come in faster
 *  (by parts: its part may have lit later; either way a touch may still be on its way), unless now: the lit frame
 *  is ending. The winner by the other count is logged with it (judged). */
static void react_judge(pp_play_t *p, bool now) {
    pp_react_t *r = &p->react;
    if (r->st != PP_RX_GO || !p->nslaps || p->go_shown <= 0) return;
    int best[2] = {-1, -1};
    float brt[2] = {1e9f, 1e9f};
    for (int m = 0; m < 2; m++)
        for (int i = 0; i < p->nslaps; i++) {
            const float rt = fmaxf(0, p->slaps[i].at - react_start(p, p->slaps[i].seat, m));
            if (rt < brt[m]) brt[m] = rt, best[m] = p->slaps[i].seat;
        }
    const int m = p->react_by_part;
    for (int k = 0; k < p->seats.n && !now; k++) {
        bool slapped = r->locked[k];
        for (int i = 0; i < p->nslaps && !slapped; i++) slapped = p->slaps[i].seat == k;
        if (!slapped && p->now < react_start(p, k, m) + brt[m] + TOUCH_LATE_S) return;
    }
    p->nslaps = 0;
    p->judged.seat[0] = best[0], p->judged.seat[1] = best[1];
    p->judged.rt[0] = brt[0], p->judged.rt[1] = brt[1];
    p->judged.serial++;
    const int seat = best[m];
    const float rt_won = brt[m];
    if (pp_react_slap(r, seat, rt_won) & PP_RX_EV_WIN) {
        p->wins[seat]++;
        face_cut(p, p->seats.s[seat].a);
        snd(p, PP_SND_REVEAL, 1);
        pp_session_react(&p->session, r->rt);
        const uint32_t cols[] = {PP_SEAT_COLORS[p->seats.s[seat].color], COLOR_CREAM};
        burst(p, seat_x(p, seat, 160), seat_y(p, seat, 160), 14, cols, 2, 260, PP_PART_CONFETTI, p->seats.s[seat].a + PP_PI, 1.6f);
        say(p, r->rt < 0.32f ? PP_LINE_FAST : PP_LINE_SUM, r->rt);
        result_changed(p);
    }
}

static void react_update(pp_play_t *p, float dt) {
    pp_react_t *r = &p->react;
    const pp_rx_state_t before = r->st;
    // no word from the display (a frame skipped, or none at all in the tests): count from the change itself
    if (r->st == PP_RX_GO && p->go_shown <= 0 && r->t > 0.25f) pp_play_go_shown(p, p->now - r->t);
    react_judge(p, r->st == PP_RX_GO && r->t + dt >= PP_REACT_GO_S);  // the lit frame ends this tick: judge what came
    const int ev = pp_react_update(r, dt);
    if (r->st != PP_RX_GO) p->nslaps = 0;
    if (ev & PP_RX_EV_HEART) snd(p, PP_SND_HEART, 1);
    if (ev & PP_RX_EV_DECOY) snd(p, PP_SND_DECOY, 1);
    if (ev & PP_RX_EV_GO) {
        p->go_shown = 0;  // set by the display when the lit frame is on the glass
        p->nslaps = 0;
        p->rot = p->rot_target;  // the lit frame at full light, whatever was turning (it is drawn whole)
        p->turn_t = -1;
        snd(p, PP_SND_SNAP_GO, 1);
    }
    if (ev & PP_RX_EV_NOBODY) say(p, PP_LINE_NOBODY, 0);
    if (ev & PP_RX_EV_OVER) {
        p->result_at = p->now;
        const int w = r->winner;
        pp_session_round(&p->session, PP_GAME_REACT);
        result_changed(p);
        snd(p, PP_SND_FANFARE_BIG, 1);
        snd(p, PP_SND_CHEER, 1.8f);
        const uint32_t cols[] = {PP_SEAT_COLORS[p->seats.s[w].color], COLOR_CREAM, COLOR_AMBER};
        burst(p, PP_C, PP_C, 36, cols, 3, 420, PP_PART_CONFETTI, 0, PP_TAU);
        yay(p, PP_YAY_HOST, PP_POSE_DONE, 2.2f, 0, 0, pp_ang_n(p->seats.s[w].a - PP_PI / 2));
        say(p, PP_LINE_SNAP_WIN, 0);
    }
    (void)before;
}

void pp_play_go_shown(pp_play_t *p, float at) {
    const int y1 = 0;
    pp_play_go_bands(p, &y1, &at, 1);
}

void pp_play_go_bands(pp_play_t *p, const int *y1, const float *on_glass, int n) {
    if (p->scene != PP_SC_REACT || p->react.st != PP_RX_GO || p->go_shown > 0 || n < 1) return;
    p->go_shown = on_glass[n - 1];
    for (int i = 0; i < p->seats.n; i++) {
        // the middle of the seat's own part (its slice, between the dead zone and the rim)
        const float y = PP_C + 165 * sinf(p->seats.s[i].a);
        int k = 0;
        while (k + 1 < n && y1[k + 1] <= y) k++;
        p->seen[i] = on_glass[k];
    }
    react_judge(p, false);
}

void pp_play_two_at_once(pp_play_t *p) {
    if (p->scene != PP_SC_REACT || p->react.st != PP_RX_GO) return;
    int free_seats[PP_SEATS_MAX], n = 0;
    for (int i = 0; i < p->seats.n; i++)
        if (!p->react.locked[i]) free_seats[n++] = i;
    if (n < 2) return;
    pp_react_tie(&p->react);
    p->nslaps = 0;
    p->tie_a = free_seats[0];
    p->tie_b = free_seats[1 + pp_randi(&p->rng, n - 1)];
    snd(p, PP_SND_ERROR, 1);
    say(p, PP_LINE_TIE, 0);
}

// ───────────────────────── bomb
static void bomb_tap(pp_play_t *p) {
    pp_bomb_t *b = &p->bomb;
    if (b->st == PP_BOMB_IDLE) {
        pp_bomb_tap(b, p->quick_bomb);
        snd(p, PP_SND_CONFIRM, 1);
        yay_here(p, PP_YAY_PEEK, PP_POSE_GREET, 1.3f, 0, 168);
        say(p, PP_LINE_BOMB_GO, 0);
    } else if (b->st == PP_BOMB_LIT) {
        // no effect on the game, but every touch answers: a spark
        static const uint32_t C2[] = {COLOR_AMBER, COLOR_CREAM};
        burst(p, PP_C, PP_C - 60, 4, C2, 2, 180, PP_PART_SPARK, 0, PP_TAU);
    } else if (b->st == PP_BOMB_PENALTY || b->st == PP_BOMB_AFTER) {
        pp_bomb_tap(b, p->quick_bomb);
        yay_hide(p);
        snd(p, PP_SND_REVEAL, 1);
    }
}

static void bomb_update(pp_play_t *p, float dt, const pp_motion_in_t *mi) {
    pp_bomb_t *b = &p->bomb;
    const int ev = pp_bomb_update(b, dt, mi->moving, p->penalties);
    if (ev & PP_BOMB_EV_TICK) snd(p, b->alt ? PP_SND_BOMB_TOCK : PP_SND_BOMB_TICK, b->urging ? 1.0f : 0.75f);
    if (ev & PP_BOMB_EV_BEEP) snd(p, PP_SND_URGE, 1);
    if (ev & PP_BOMB_EV_NAG) {
        yay_here(p, PP_YAY_PEEK, PP_POSE_REMINDER, 1.2f, 0, 168);
        say(p, PP_LINE_PASS_IT, 0);
    }
    if (ev & PP_BOMB_EV_BOOM) {
        snd(p, PP_SND_BOOM, 1);
        pp_session_round(&p->session, PP_GAME_BOMB);
        result_changed(p);
        static const uint32_t D[] = {COLOR_CREAM, COLOR_AMBER, COLOR_CORAL, COLOR_DEBRIS};
        static const uint32_t S1[] = {COLOR_GREY};
        burst(p, PP_C, PP_C, 28, D, 4, 520, PP_PART_DEBRIS, 0, PP_TAU);
        for (int i = 0; i < 6; i++)
            burst(p, PP_C + pp_randr(&p->rng, -40, 40), PP_C + pp_randr(&p->rng, -40, 40), 1, S1, 1, 60, PP_PART_SMOKE, 0, PP_TAU);
        face_cut(p, mi->holder);
    }
    if (b->st == PP_BOMB_BOOM && b->t >= 0.38f && b->t - dt < 0.38f) {
        yay(p, PP_YAY_HOST, PP_POSE_HELP, 2.4f, 0, 0, p->rot_target);
        say(p, PP_LINE_BOOM, 0);
    }
    if (ev & PP_BOMB_EV_PENALTY) {
        snd(p, PP_SND_REVEAL, 1);
        yay_here(p, PP_YAY_PEEK, PP_POSE_SPEAKING, 2.2f, 0, 165);
        say(p, PP_LINE_PENALTY, (float)b->penalty);
    }
}

// ───────────────────────── ten
static void ten_stop_feedback(pp_play_t *p) {
    const pp_ten_t *t = &p->ten;
    snd(p, PP_SND_TEN_STOP, 1);
    pp_session_round(&p->session, PP_GAME_TEN);
    pp_session_ten(&p->session, t->res);
    result_changed(p);
    static const uint32_t C4[] = {COLOR_CORAL, COLOR_AMBER, COLOR_CREAM, COLOR_SAGE};
    switch (pp_ten_rate(t->res)) {
    case PP_TEN_PERFECT:
        snd(p, PP_SND_FANFARE_BIG, 1);
        snd(p, PP_SND_CHEER, 1.4f);
        burst(p, PP_C, PP_C, 34, C4, 4, 420, PP_PART_CONFETTI, 0, PP_TAU);
        yay_here(p, PP_YAY_PEEK, PP_POSE_DONE, 1.8f, 0, -126);
        say(p, PP_LINE_PERFECT, 0);
        break;
    case PP_TEN_CLOSE:
        snd(p, PP_SND_DING, 1);
        yay_here(p, PP_YAY_PEEK, PP_POSE_GREET, 1.4f, 0, -126);
        say(p, PP_LINE_CLOSE, 0);
        break;
    case PP_TEN_OK:
        say(p, PP_LINE_OK, 0);
        break;
    default:
        say(p, PP_LINE_AGAIN, 0);
        break;
    }
}

static void ten_tap(pp_play_t *p) {
    const pp_ten_state_t st = p->ten.st;
    const int ev = pp_ten_tap(&p->ten);
    if (st == PP_TEN_IDLE) snd(p, PP_SND_TEN_START, 1);
    if (ev & PP_TEN_EV_STOP) ten_stop_feedback(p);
    if (st == PP_TEN_FINAL && p->ten.multi) face_cut(p, p->seats.s[0].a);
}

static void ten_update(pp_play_t *p, float dt) {
    const int ev = pp_ten_update(&p->ten, dt);
    if (ev & PP_TEN_EV_SECOND) snd(p, PP_SND_TEN_TICK, 1);
    if (ev & PP_TEN_EV_STOP) ten_stop_feedback(p);
    if (ev & PP_TEN_EV_NEXT) {
        face_cut(p, p->seats.s[p->ten.turn].a);
        snd(p, PP_SND_TICK, 1);
    }
    if (ev & PP_TEN_EV_FINAL) {
        p->result_at = p->now;
        const int w = p->ten.winner;
        if (w >= 0) {
            p->wins[w]++;
            face_cut(p, p->seats.s[w].a);
            snd(p, PP_SND_FANFARE, 1);
            snd(p, PP_SND_CHEER, 1.3f);
            const uint32_t cols[] = {PP_SEAT_COLORS[p->seats.s[w].color], COLOR_CREAM, COLOR_AMBER};
            burst(p, seat_x(p, w, 150), seat_y(p, w, 150), 30, cols, 3, 380, PP_PART_CONFETTI, p->seats.s[w].a + PP_PI, 2);
            yay_here(p, PP_YAY_PEEK, PP_POSE_DONE, 2, 0, -120);
            say(p, PP_LINE_BEST, 0);
            result_changed(p);
        }
    }
}

// ───────────────────────── settings
static void settings_rim(pp_play_t *p, float da) {
    p->set_acc += da;
    if (fabsf(p->set_acc) < 0.38f) return;
    const int d = p->set_acc > 0 ? 1 : -1;
    p->set_acc = 0;
    bool changed = false;
    switch (p->set_sel) {
    case 0: {
        const int n = pp_clampi(p->players + d, 2, PP_SEATS_MAX);
        if (n != p->players) {
            p->players = n;
            pp_seats_even(&p->seats, n);
            memset(p->wins, 0, sizeof(p->wins));
            changed = true;
        }
        break;
    }
    case 1: {
        const int n = pp_clampi(p->dice_n + d, 1, PP_DICE_MAX);
        if (n != p->dice_n) p->dice_n = n, changed = true;
        break;
    }
    case 2: {
        const int n = pp_clampi(p->volume + d, 0, 10);
        if (n != p->volume) p->volume = n, changed = true;
        break;
    }
    default: {
        const bool on = d > 0;
        if (on != p->penalties) p->penalties = on, changed = true;
        break;
    }
    }
    snd(p, PP_SND_TICK, changed ? 1 : 0.3f);
}

static void settings_tap(pp_play_t *p, float x, float y) {
    const float c = cosf(-p->rot), s = sinf(-p->rot), dx = x - PP_C, dy = y - PP_C;
    const float qx = dx * c - dy * s, qy = dx * s + dy * c;
    if (fabsf(qy + 8) >= 80) return;
    const int i = pp_clampi((int)floorf((qx + 184) / 92), 0, 3);
    if (i == p->set_sel && i == 0) {
        snd(p, PP_SND_CONFIRM, 1);
        p->seats_next = p->back;
        pp_play_go(p, PP_SC_SEATS);
        return;
    }
    if (i == p->set_sel && i == 3) {
        p->penalties = !p->penalties;
        snd(p, PP_SND_CONFIRM, 1);
        return;
    }
    p->set_sel = i;
    snd(p, PP_SND_CONFIRM, 1);
}

// ───────────────────────── input
/** The screen comes back on (from off, or turned face up): its first frame is drawn while still dark, then it comes
 *  up like a new scene, so the picture from before sleep is never shown lit. */
static void wake(pp_play_t *p) {
    p->off = p->dimmed = false;
    p->fade = -1;
}

bool pp_play_touch_down(pp_play_t *p, float x, float y) {
    (void)x, (void)y;
    p->idle = p->quiet = 0;
    p->resting = false;
    if (p->face_down) return false;
    if (p->off) {
        wake(p);
        return false;  // the first touch only wakes the screen
    }
    p->dimmed = false;
    return true;
}

void pp_play_tap(pp_play_t *p, float x, float y, float at) {
    p->idle = p->quiet = 0;
    if (p->sheet) {
        p->sheet = false;
        return;
    }
    switch (p->scene) {
    case PP_SC_MENU: menu_tap(p, x, y); break;
    case PP_SC_SEATS: seats_tap(p, x, y); break;
    case PP_SC_DICE: dice_tap(p, x, y); break;
    case PP_SC_WHEEL:
        if (p->wheel_st != PP_WHEEL_SPINNING) wheel_spin(p, (pp_randf(&p->rng) < 0.5f ? -1 : 1) * pp_randr(&p->rng, 13, 19));
        break;
    case PP_SC_REACT: react_tap(p, x, y, at); break;
    case PP_SC_BOMB: bomb_tap(p); break;
    case PP_SC_TEN: ten_tap(p); break;
    case PP_SC_SETTINGS: settings_tap(p, x, y); break;
    case PP_SC_CHARGE: snd(p, PP_SND_TICK, 0.5f); break;
    default: break;
    }
}

float pp_play_rim_min(const pp_play_t *p) {
    switch (p->scene) {
    case PP_SC_MENU:
    case PP_SC_DICE:
    case PP_SC_TEN:
    case PP_SC_SETTINGS: return 150;
    case PP_SC_WHEEL: return 92;  // the needle and the slices; the small middle is left for swipes
    default: return 1000;         // no turning along the edge here
    }
}

void pp_play_rim_start(pp_play_t *p, float x, float y) {
    p->idle = p->quiet = 0;
    switch (p->scene) {
    case PP_SC_MENU:
        p->menu_held = true;
        p->menu_vel = 0;
        p->menu_snapping = false;
        face(p, pp_point_angle(x, y), true);
        break;
    case PP_SC_DICE: p->rim_acc = 0; break;
    case PP_SC_WHEEL:
        if (p->wheel_st != PP_WHEEL_SPINNING) p->wheel_st = PP_WHEEL_DRAGGING;
        break;
    case PP_SC_TEN: p->set_acc = 0; break;
    case PP_SC_SETTINGS: p->set_acc = 0; break;
    default: break;
    }
}

void pp_play_rim_move(pp_play_t *p, float da) {
    switch (p->scene) {
    case PP_SC_MENU: p->menu_pos -= da / MENU_STEP; break;
    case PP_SC_DICE: dice_rim(p, da); break;
    case PP_SC_WHEEL:
        if (p->wheel_st == PP_WHEEL_DRAGGING && (pp_wheel_drag(&p->wheel, &p->seats, da) & PP_WHEEL_TICK)) snd(p, PP_SND_WHEEL_TICK, 1);
        break;
    case PP_SC_TEN:
        if (p->ten.st != PP_TEN_IDLE) break;
        p->set_acc += da;
        if (fabsf(p->set_acc) > 0.7f) {
            p->set_acc = 0;
            p->ten.mode = 1 - p->ten.mode;
            snd(p, PP_SND_TICK, 1);
        }
        break;
    case PP_SC_SETTINGS: settings_rim(p, da); break;
    default: break;
    }
}

void pp_play_rim_end(pp_play_t *p, float w) {
    switch (p->scene) {
    case PP_SC_MENU:
        p->menu_held = false;
        p->menu_vel = pp_clampf(-w / MENU_STEP, -8, 8);
        break;
    case PP_SC_WHEEL:
        if (p->wheel_st == PP_WHEEL_DRAGGING) {
            if (fabsf(w) > 4) wheel_spin(p, w);
            else p->wheel_st = PP_WHEEL_IDLE;
        }
        break;
    default: break;
    }
}

static bool swipe_ok(const pp_play_t *p) {
    switch (p->scene) {
    case PP_SC_DICE: return p->dice_st == PP_DICE_IDLE || p->dice_st == PP_DICE_RESULT;
    case PP_SC_WHEEL: return p->wheel_st == PP_WHEEL_IDLE || p->wheel_st == PP_WHEEL_RESULT;
    case PP_SC_REACT: return p->react.st == PP_RX_READY || p->react.st == PP_RX_WIN || p->react.st == PP_RX_OVER;
    case PP_SC_BOMB: return p->bomb.st != PP_BOMB_LIT && p->bomb.st != PP_BOMB_BOOM;
    case PP_SC_TEN: return p->ten.st != PP_TEN_RUN;
    default: return true;
    }
}

void pp_play_swipe(pp_play_t *p, int dir) {
    p->idle = p->quiet = 0;
    if (!swipe_ok(p)) return;
    const bool game = p->scene == PP_SC_MENU || (p->scene >= PP_SC_DICE && p->scene <= PP_SC_TEN);
    if (dir == PP_G_SWIPE_LEFT || dir == PP_G_SWIPE_RIGHT) {
        if (p->scene == PP_SC_SETTINGS && dir == PP_G_SWIPE_RIGHT) return pp_play_go(p, p->back);
        if (p->scene == PP_SC_SCORES && dir == PP_G_SWIPE_LEFT) return pp_play_go(p, p->back);
        if (game) {
            p->back = p->scene;
            snd(p, PP_SND_SWISH, 1);
            pp_play_go(p, dir == PP_G_SWIPE_LEFT ? PP_SC_SETTINGS : PP_SC_SCORES);
        }
        return;
    }
    if (dir == PP_G_SWIPE_DOWN) {
        p->sheet = !p->sheet;
        p->sheet_t = p->now;
        snd(p, PP_SND_SWISH, 0.6f);
    } else if (dir == PP_G_SWIPE_UP && p->sheet) {
        p->sheet = false;
    }
}

void pp_play_key_short(pp_play_t *p) {
    p->idle = p->quiet = 0;
    if (p->off) {
        wake(p);
        return;
    }
    if (p->face_down) return;
    if (p->sheet) {
        p->sheet = false;
        return;
    }
    switch (p->scene) {
    case PP_SC_SETTINGS:
    case PP_SC_SCORES: pp_play_go(p, p->back); return;
    case PP_SC_BOMB:
        if (p->bomb.st == PP_BOMB_LIT) {
            pp_bomb_cancel(&p->bomb);
            snd(p, PP_SND_FIZZ, 1);
            return;
        }
        if (p->bomb.st == PP_BOMB_PENALTY) {
            pp_bomb_cancel(&p->bomb);
            yay_hide(p);
            return;
        }
        break;
    default: break;
    }
    if (p->scene >= PP_SC_SEATS && p->scene <= PP_SC_TEN) {
        snd(p, PP_SND_CLOSE, 1);
        for (int i = 0; i < MENU_GAMES; i++)
            if (MENU_SCENES[i] == p->scene) p->menu_pos = (float)i, p->menu_last = i;
        pp_play_go(p, PP_SC_MENU);
    }
}

void pp_play_face_down(pp_play_t *p, bool down) {
    if (down == p->face_down) return;
    p->face_down = down;
    p->idle = p->quiet = 0;
    if (!down) {
        wake(p);
        snd(p, PP_SND_CONFIRM, 1);
    }
}

void pp_play_battery(pp_play_t *p, int level, int pct) {
    p->battery_pct = pct;
    if (level == p->battery) return;
    const int before = p->battery;
    p->battery = level;
    if (level == 1 && before == 0) {
        p->bat_flash = p->now;
        yay_here(p, PP_YAY_PEEK, PP_POSE_REMINDER, 1.4f, 0, -118);
        say(p, PP_LINE_LOW_BAT, 0);
    }
    if (level == 0 && p->scene == PP_SC_CHARGE) {
        snd(p, PP_SND_CONFIRM, 1);
        pp_play_go(p, PP_SC_MENU);
    }
}

float pp_play_light(const pp_play_t *p) {
    float k = pp_ease_out(fmaxf(p->fade, 0));
    if (p->turn_t >= 0) k = fminf(k, turn_light(p->turn_t));
    return pp_clampf(k, 0, 1);
}

// three beats and a quarter: it stops at its brightest, so holding there is no jump
#define PULSE_S ((PP_PI / 2 + 6 * PP_PI) / 7)

float pp_play_pulse(const pp_play_t *p) {
    const float t = p->now - p->result_at;
    return t < PULSE_S ? 0.5f + 0.5f * sinf(t * 7) : 1;
}

bool pp_play_busy(const pp_play_t *p) {
    switch (p->scene) {
    case PP_SC_DICE: return p->dice_st == PP_DICE_ROLL || p->dice_st == PP_DICE_POP || p->dice_st == PP_DICE_ARRANGE;
    case PP_SC_WHEEL: return p->wheel_st == PP_WHEEL_SPINNING || p->wheel_st == PP_WHEEL_DRAGGING;
    case PP_SC_REACT: return p->react.st == PP_RX_WAIT || p->react.st == PP_RX_GO || p->react.st == PP_RX_DECOY;
    case PP_SC_BOMB: return p->bomb.st == PP_BOMB_LIT || p->bomb.st == PP_BOMB_BOOM;
    case PP_SC_TEN: return p->ten.st == PP_TEN_RUN;
    case PP_SC_SEATS: return p->color_at >= 0;
    default: return false;
    }
}

bool pp_play_in_round(const pp_play_t *p) {
    if (pp_play_busy(p)) return true;
    switch (p->scene) {
    case PP_SC_REACT: return p->react.st == PP_RX_WIN || p->react.st == PP_RX_TIE;  // between points
    case PP_SC_TEN:  // between players' turns, until the last one's
        return p->ten.multi && p->ten.st != PP_TEN_FINAL && (p->ten.turn > 0 || p->ten.st == PP_TEN_SHOWN);
    default: return false;
    }
}

// ───────────────────────── time
void pp_play_tick(pp_play_t *p, float dt, const pp_motion_in_t *mi) {
    static const pp_motion_in_t none = {.holder = 1.5707963f};
    if (!mi) mi = &none;
    if (dt > 0.1f) dt = 0.1f;  // a stall is not a jump in the game
    if (mi->events & PP_MO_FACE_DOWN) pp_play_face_down(p, true);
    if (mi->events & PP_MO_FACE_UP) pp_play_face_down(p, false);
    if (mi->events & PP_MO_LIFT) {
        p->idle = p->quiet = 0;
        if (p->off) wake(p);
    }
    if (p->face_down) return;  // quiet: everything waits, the bomb too
    if (p->off) {
        p->now += dt;
        return;
    }
    p->now += dt;
    p->scene_t += dt;
    p->session.seconds += dt;
    // a new scene stays dark through the tick it is first drawn in, then comes up from the start: the light moves at
    // most a frame's worth a tick, so a slow first frame does not eat the fade (nor the old scene show meanwhile)
    const float step = fminf(dt, LIGHT_STEP_S);
    p->fade = p->fade < 0 ? 0 : fminf(1, p->fade + step / 0.22f);
    // a turn: dark, then the middle snaps to whoever it now faces, then light again
    if (p->turn_t < 0 && fabsf(p->rot_target - p->rot) > 0.001f) p->turn_t = 0;
    if (p->turn_t >= 0) {
        const float before = p->turn_t;
        p->turn_t += step;
        // only at the dark point (a later change of who it faces waits for the next turn), and dark for that tick:
        // the turned screen is drawn unseen
        if (before < TURN_OUT_S && p->turn_t >= TURN_OUT_S) p->rot = p->rot_target, p->turn_t = TURN_OUT_S;
        if (p->turn_t >= TURN_OUT_S + TURN_IN_S) p->turn_t = -1;
    }

    switch (p->scene) {
    case PP_SC_MENU: menu_update(p, dt); break;
    case PP_SC_SEATS: seats_update(p, dt); break;
    case PP_SC_DICE: dice_update(p, dt, mi); break;
    case PP_SC_WHEEL: wheel_update(p, dt); break;
    case PP_SC_REACT: react_update(p, dt); break;
    case PP_SC_BOMB: bomb_update(p, dt, mi); break;
    case PP_SC_TEN: ten_update(p, dt); break;
    default: break;
    }
    parts_update(p, dt);
    if (p->yay.mode != PP_YAY_HIDDEN && p->now > p->yay.until) yay_hide(p);

    // sleep: dim at 60 s, Yay rests for the last 3 s, off at the shared screen-off time but not before 63 s (30 s
    // by default since P22; 0 never). Never in a round, from its start to its result: the time starts again at the
    // result. A round nobody has touched or moved for 5 minutes was left (react keeps lighting up for no one) and
    // sleeps like the menu.
    p->quiet = mi->held ? 0 : p->quiet + dt;
    const bool round = pp_play_in_round(p) && p->quiet < ROUND_QUIET_S;
    if (round || mi->held || p->scene == PP_SC_CHARGE) p->idle = 0;
    else p->idle += dt;
    if (p->idle > IDLE_DIM) p->dimmed = true;
    if (p->idle_off > 0) {
        const float rest_at = fmaxf(p->idle_off - IDLE_REST_S, IDLE_DIM);
        if (p->idle > rest_at && !p->resting) {
            p->resting = true;
            p->idle = rest_at;
            yay_here(p, PP_YAY_HOST, PP_POSE_RESTING, IDLE_REST_S, 0, 0);
        }
        if (p->idle > rest_at + IDLE_REST_S) {
            p->off = true;
            yay_hide(p);
        }
    }
    if (p->battery == 2 && !pp_play_busy(p) && p->scene != PP_SC_CHARGE) pp_play_go(p, PP_SC_CHARGE);
}
