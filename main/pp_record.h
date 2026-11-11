// What one session leaves behind (docs/product/party-play.md "交付到动态和资产"): the result Yay reads once when the
// app goes home (yk_app_set_result_summary: a sentence, the numbers for Yay's ring, the games), and a party.session
// record for the outbox (yk_app_record), kept only if the user agreed to records. Numbers only, never people.
// Plain C, host tested.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { PP_GAME_DICE, PP_GAME_WHEEL, PP_GAME_REACT, PP_GAME_BOMB, PP_GAME_TEN, PP_GAME_COUNT } pp_game_t;

/** "dice", "wheel", "react", "bomb", "ten" (the launch parameter and the record's keys). */
const char *pp_game_key(pp_game_t game);
/** -1 for an unknown key. */
int pp_game_from_key(const char *key);

typedef struct {
    bool has;
    float value;
} pp_highlight_t;

typedef struct {
    uint16_t rounds[PP_GAME_COUNT];  // dice rolled, spins, react matches, bombs gone off, ten-second tries
    int players;                     // seats in use at the end (0: never seated)
    float seconds;                   // played (screen on), not counting time asleep
    pp_highlight_t triple;           // three dice the same: the face
    pp_highlight_t perfect_ten;      // ten seconds within 0.05: the time stopped
    pp_highlight_t fast_react;       // a reaction under 0.25 s: the best time
} pp_session_t;

void pp_session_reset(pp_session_t *s);
void pp_session_round(pp_session_t *s, pp_game_t game);
void pp_session_dice(pp_session_t *s, int n, int face_if_all_same);  // face 0: not all the same
void pp_session_ten(pp_session_t *s, float stopped);
void pp_session_react(pp_session_t *s, float rt);
int pp_session_total_rounds(const pp_session_t *s);
/** Long enough to record: at least 3 minutes or 3 rounds (not just a look). */
bool pp_session_worth(const pp_session_t *s);

/** The party.session record's data (JSON object). Returns the length or -1 when it does not fit. */
int pp_session_record_json(const pp_session_t *s, char *out, size_t size);
/** One sentence for Yay to say ("转盘转了 3 次，抢拍比了 1 场"); english when en. */
int pp_session_summary(const pp_session_t *s, bool en, char *out, size_t size);
/** The numbers Yay draws in its ring when it is back (P3, yk_summary_t): rounds, players, minutes, those that apply,
 *  at most 3. Returns how many. */
int pp_session_stats(const pp_session_t *s, bool en, char value[3][16], char label[3][24]);
/** The result's own data next to the summary: {"games":{...},"players":n}. Length or -1. */
int pp_session_data_json(const pp_session_t *s, char *out, size_t size);
