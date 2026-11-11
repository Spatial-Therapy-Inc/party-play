// Party Play's whole state (docs/product/party-play-ui.md): scenes, who the middle faces, the menu, seating, the
// five games, settings and scores, when Yay comes on, particles, sleep and battery rules. Plain C and host tested:
// the device feeds it touches, keys, motion and time, then draws what it says and plays the sounds it asks for.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "pp_bomb.h"
#include "pp_dice.h"
#include "pp_launch.h"
#include "pp_react.h"
#include "pp_record.h"
#include "pp_seats.h"
#include "pp_ten.h"
#include "pp_wheel.h"

typedef enum {
    PP_SC_MENU,
    PP_SC_SEATS,
    PP_SC_DICE,
    PP_SC_WHEEL,
    PP_SC_REACT,
    PP_SC_BOMB,
    PP_SC_TEN,
    PP_SC_SETTINGS,
    PP_SC_SCORES,
    PP_SC_CHARGE,
    PP_SC_COUNT,
} pp_scene_t;

/** Sounds the device should make (docs/product/party-play-ui.md "声音清单"). */
typedef enum {
    PP_SND_CONFIRM,
    PP_SND_TICK,
    PP_SND_ERROR,
    PP_SND_DICE_HIT,
    PP_SND_DICE_WALL,
    PP_SND_SETTLE,
    PP_SND_REVEAL,
    PP_SND_FANFARE,
    PP_SND_FANFARE_BIG,
    PP_SND_CHEER,
    PP_SND_WHEEL_TICK,
    PP_SND_DRUMROLL,
    PP_SND_DING,
    PP_SND_BOMB_TICK,
    PP_SND_BOMB_TOCK,
    PP_SND_URGE,
    PP_SND_BOOM,
    PP_SND_FIZZ,
    PP_SND_HEART,
    PP_SND_SNAP_GO,
    PP_SND_DECOY,
    PP_SND_TEN_START,
    PP_SND_TEN_TICK,
    PP_SND_TEN_STOP,
    PP_SND_SEAT,
    PP_SND_UNSEAT,
    PP_SND_OPEN,
    PP_SND_CLOSE,
    PP_SND_SWISH,
    PP_SND_COUNT,
} pp_sound_t;

/** Yay's offline lines (docs/product/party-play-ui.md "离线台词"); voiced only once the TTS terms are cleared. */
typedef enum {
    PP_LINE_HELLO,
    PP_LINE_DICE_GO,
    PP_LINE_SUM,
    PP_LINE_PAIR,
    PP_LINE_TRIPLE,
    PP_LINE_WHEEL_GO,
    PP_LINE_ITS_YOU,
    PP_LINE_SEATS,
    PP_LINE_SNAP_GO,
    PP_LINE_TOO_EARLY,
    PP_LINE_FAST,
    PP_LINE_SNAP_WIN,
    PP_LINE_TIE,
    PP_LINE_NOBODY,
    PP_LINE_BOMB_GO,
    PP_LINE_PASS_IT,
    PP_LINE_BOOM,
    PP_LINE_PENALTY,
    PP_LINE_TEN_GO,
    PP_LINE_PERFECT,
    PP_LINE_CLOSE,
    PP_LINE_OK,
    PP_LINE_AGAIN,
    PP_LINE_BEST,
    PP_LINE_LOW_BAT,
    PP_LINE_PLUG_IN,
    PP_LINE_COUNT,
} pp_line_t;

typedef enum { PP_POSE_GREET, PP_POSE_DONE, PP_POSE_HELP, PP_POSE_REMINDER, PP_POSE_RESTING, PP_POSE_SPEAKING } pp_pose_t;
typedef enum { PP_YAY_HIDDEN, PP_YAY_HOST, PP_YAY_PEEK } pp_yay_mode_t;

typedef struct {
    pp_yay_mode_t mode;
    pp_pose_t pose;
    float until;   // play time it leaves (INFINITY: stays)
    float ox, oy;  // peek: offset in the frame of the person it faces
    float rot;     // facing (rotation of the image)
    uint32_t serial;
} pp_yay_t;

typedef enum { PP_OUT_SOUND, PP_OUT_LINE, PP_OUT_RESULT, PP_OUT_SCENE } pp_out_kind_t;
typedef struct {
    uint8_t kind;  // pp_out_kind_t
    uint8_t id;    // sound / line / scene
    float gain;    // sound: 0..1; line: a number to say (sum, seconds)
} pp_out_t;

#define PP_PARTS_MAX 40
typedef enum { PP_PART_CONFETTI, PP_PART_SPARK, PP_PART_DEBRIS, PP_PART_SMOKE } pp_part_kind_t;
typedef struct {
    float x, y, vx, vy, r, vr, life, s;
    uint32_t color;
    uint8_t kind;
} pp_part_t;

/** Motion events the game reacts to (pp_motion_in_t.events), from yaykit_motion's (pp_motion_map.c). */
enum {
    PP_MO_LIFT = 1 << 0,       // picked up from rest
    PP_MO_PUT = 1 << 1,        // at rest again after moving
    PP_MO_SHAKE = 1 << 2,      // shaking (set every tick while it goes on)
    PP_MO_FACE_DOWN = 1 << 3,  // glass down: quiet
    PP_MO_FACE_UP = 1 << 4,    // turned back over
};

/** What the device tells the model about motion each tick (pp_motion_map.c, from yaykit_motion). */
typedef struct {
    float fx, fy;   // push on the dice in the screen plane, px/s^2
    float jolt;     // 0..1
    bool held;      // in a hand
    bool moving;    // moved within the last second
    float holder;   // angle of whoever holds it
    float holder_conf;
    int events;     // PP_MO_* flags
} pp_motion_in_t;

typedef enum { PP_DICE_IDLE, PP_DICE_ROLL, PP_DICE_POP, PP_DICE_ARRANGE, PP_DICE_RESULT } pp_dice_state_t;
typedef enum { PP_WHEEL_IDLE, PP_WHEEL_DRAGGING, PP_WHEEL_SPINNING, PP_WHEEL_RESULT } pp_wheel_state_t;

typedef struct {
    // settings (kept in NVS by the device)
    int players, dice_n, volume;  // volume 0-10 (shared with Yay once P10 lands)
    bool penalties, en, quick_bomb;

    float now;  // play time, s
    uint32_t rng;

    // who the middle faces: a turn goes dark, snaps, lights again (the whole screen redrawn once, unseen)
    float rot, rot_target;
    float turn_t;  // time into the turn; < 0 none

    pp_scene_t scene, back;  // back: where settings / scores return to
    float scene_t, fade;

    // seats in use this session
    pp_seats_t seats;
    uint16_t wins[PP_SEATS_MAX];
    pp_session_t session;
    bool result_dirty;

    // menu
    float menu_pos, menu_vel, menu_snap;
    bool menu_snapping, menu_held, first_menu;
    int menu_last;

    // seating page
    pp_seats_t seating;
    float seat_born[PP_SEATS_MAX];
    pp_scene_t seats_next;
    int color_at;
    float color_t;

    // dice
    pp_dice_t dice;
    pp_dice_state_t dice_st;
    float reveal_t, roller, rim_acc;
    float from[PP_DICE_MAX][3], scale[PP_DICE_MAX];
    bool dice_special;
    int rolls;

    // wheel
    pp_wheel_t wheel;
    pp_wheel_state_t wheel_st;
    float flash[PP_SEATS_MAX];
    int spins;

    // react
    pp_react_t react;
    float go_shown;  // play time the lit frame was all on the glass (0: not yet)
    float seen[PP_SEATS_MAX];  // when each seat's part of it was: the screen is sent in bands from the top
    // Whose start counts: false (default), everyone's from the frame all on the glass, as in 1.0.3; true, each seat's
    // from its own part. Players may well see the first band to light, not their own slice, so the second waits for
    // people sitting top and bottom to compare; each judgement logs both (judged).
    bool react_by_part;
    struct {
        int seat[2];  // the winner by the whole frame [0] and by each seat's part [1]
        float rt[2];
        uint32_t serial;
    } judged;
    struct {
        int seat;
        float at;
    } slaps[PP_SEATS_MAX];  // slaps on the lit frame, judged once nobody could still be faster
    int nslaps;
    float result_at;  // when the result on screen came (the wheel's seat, a react match's winner, ten seconds' best)
    int tie_a, tie_b;

    // bomb
    pp_bomb_t bomb;

    // ten
    pp_ten_t ten;

    // settings page
    int set_sel;
    float set_acc;

    // status sheet (swipe down)
    bool sheet;
    float sheet_t;

    pp_yay_t yay;
    pp_part_t parts[PP_PARTS_MAX];
    int nparts;

    // screen and power
    float idle_off;  // seconds without input before the screen goes off (the shared preference); 0 never
    float idle;      // towards dimming and the screen off; held at 0 in a round
    float quiet;     // since the last touch, key or movement: a round left this long sleeps after all
    bool dimmed, off, resting, face_down;
    int battery;  // 0 ok, 1 low (< 15%), 2 critical (< 5%)
    int battery_pct;  // -1 unknown
    float bat_flash;

    pp_out_t out[48];
    int nout;
} pp_play_t;

void pp_play_init(pp_play_t *p, uint32_t seed);
/** Yay opened the app with parameters (call once, after init and settings). */
void pp_play_launch(pp_play_t *p, const pp_launch_params_t *lp);
/** Time passes (dt s) with the motion state. */
void pp_play_tick(pp_play_t *p, float dt, const pp_motion_in_t *mi);

/** A touch went down (any): wakes the screen. Returns false when it only woke it (ignore the rest of this touch). */
bool pp_play_touch_down(pp_play_t *p, float x, float y);
void pp_play_tap(pp_play_t *p, float x, float y, float at);  // at: play time of the touch (for the reaction game)
void pp_play_rim_start(pp_play_t *p, float x, float y);
void pp_play_rim_move(pp_play_t *p, float da);
void pp_play_rim_end(pp_play_t *p, float w);
void pp_play_swipe(pp_play_t *p, int dir);  // pp_gesture_ev_t swipe values
/** The minimum radius a drag must start at to turn along the edge, for the current scene. */
float pp_play_rim_min(const pp_play_t *p);
/** The reaction game's lit frame reached the glass all at once at `at`. */
void pp_play_go_shown(pp_play_t *p, float at);
/** The same, band by band as the display sent it: band k (rows from y1[k] to the next band) was on the glass at
 *  on_glass[k]. Reactions count from the last band (the whole frame) or, with react_by_part, each from when its own
 *  part of the screen lit. */
void pp_play_go_bands(pp_play_t *p, const int *y1, const float *on_glass, int n);
/** The touch controller's point jumped mid-press during the reaction game: two people at once. */
void pp_play_two_at_once(pp_play_t *p);

void pp_play_key_short(pp_play_t *p);
void pp_play_face_down(pp_play_t *p, bool down);
void pp_play_battery(pp_play_t *p, int level, int pct);
/** Console / tests: go to a scene directly. */
void pp_play_go(pp_play_t *p, pp_scene_t scene);

/** True while something is moving that a scene change must not cut (dice rolling, spinning, fuse lit, timing,
 *  waiting). */
bool pp_play_busy(const pp_play_t *p);
/** True from a round's start to its result, the screen kept on: dice from the throw to the sum, the wheel from the
 *  spin to the seat, a react match to its winner, ten seconds to the last player's turn, the bomb lit. */
bool pp_play_in_round(const pp_play_t *p);
/** The screen's light, 0..1, multiplying the brightness: a new scene comes up from dark (0.22 s), a turn goes dark
 *  (0.12 s), snaps and comes back (0.15 s). The panel's brightness does it, so the scene is drawn once, unseen,
 *  instead of every frame of a fade or a turn. */
float pp_play_light(const pp_play_t *p);
/** The winner's highlight on a result: pulses 0..1 for its first ~3 s (three beats), then holds at 1, so a result
 *  left on the table stops redrawing. */
float pp_play_pulse(const pp_play_t *p);
/** Takes the outputs queued since the last call. */
int pp_play_take(pp_play_t *p, pp_out_t *out, int max);
/** Short scene name for logs. */
const char *pp_scene_name(pp_scene_t s);
const char *pp_line_text(pp_line_t line, bool en);
