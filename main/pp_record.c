#include "pp_record.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char *const KEYS[PP_GAME_COUNT] = {"dice", "wheel", "react", "bomb", "ten"};

const char *pp_game_key(pp_game_t game) { return game < PP_GAME_COUNT ? KEYS[game] : ""; }

int pp_game_from_key(const char *key) {
    for (int i = 0; i < PP_GAME_COUNT; i++)
        if (key && !strcmp(key, KEYS[i])) return i;
    return -1;
}

void pp_session_reset(pp_session_t *s) { memset(s, 0, sizeof(*s)); }

void pp_session_round(pp_session_t *s, pp_game_t game) {
    if (game < PP_GAME_COUNT && s->rounds[game] < 999) s->rounds[game]++;
}

void pp_session_dice(pp_session_t *s, int n, int face_if_all_same) {
    // a triple needs three dice; a higher face is the better story
    if (n == 3 && face_if_all_same > 0 && (!s->triple.has || face_if_all_same > s->triple.value)) {
        s->triple.has = true;
        s->triple.value = (float)face_if_all_same;
    }
}

void pp_session_ten(pp_session_t *s, float stopped) {
    const float d = fabsf(stopped - 10);
    if (d <= 0.05f && (!s->perfect_ten.has || d < fabsf(s->perfect_ten.value - 10))) {
        s->perfect_ten.has = true;
        s->perfect_ten.value = stopped;
    }
}

void pp_session_react(pp_session_t *s, float rt) {
    if (rt > 0 && rt < 0.25f && (!s->fast_react.has || rt < s->fast_react.value)) {
        s->fast_react.has = true;
        s->fast_react.value = rt;
    }
}

int pp_session_total_rounds(const pp_session_t *s) {
    int n = 0;
    for (int i = 0; i < PP_GAME_COUNT; i++) n += s->rounds[i];
    return n;
}

bool pp_session_worth(const pp_session_t *s) { return s->seconds >= 180 || pp_session_total_rounds(s) >= 3; }

/** Appends to a buffer; remembers overflow. */
typedef struct {
    char *p;
    size_t size, len;
    bool over;
} buf_t;

static void put(buf_t *b, const char *fmt, ...) {
    if (b->over) return;
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(b->p + b->len, b->size - b->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= b->size - b->len) {
        b->over = true;
        return;
    }
    b->len += (size_t)n;
}

static int done(buf_t *b) { return b->over ? -1 : (int)b->len; }

static int minutes_of(const pp_session_t *s) { return (int)lroundf(s->seconds / 60); }

static void games_json(buf_t *b, const pp_session_t *s) {
    put(b, "{");
    bool first = true;
    for (int i = 0; i < PP_GAME_COUNT; i++) {
        if (!s->rounds[i]) continue;
        put(b, "%s\"%s\":%u", first ? "" : ",", KEYS[i], s->rounds[i]);
        first = false;
    }
    put(b, "}");
}

int pp_session_record_json(const pp_session_t *s, char *out, size_t size) {
    buf_t b = {out, size, 0, false};
    if (size) out[0] = 0;
    put(&b, "{\"minutes\":%d,", minutes_of(s));
    if (s->players >= 1) put(&b, "\"players\":%d,", s->players);
    else put(&b, "\"players\":null,");
    put(&b, "\"games\":");
    games_json(&b, s);
    put(&b, ",\"highlights\":[");
    // at most 3, in this order: triple, ten seconds, reaction
    bool first = true;
    if (s->triple.has) put(&b, "{\"kind\":\"triple\",\"value\":%d}", (int)s->triple.value), first = false;
    if (s->perfect_ten.has) put(&b, "%s{\"kind\":\"perfect_ten\",\"value\":%.2f}", first ? "" : ",", s->perfect_ten.value), first = false;
    if (s->fast_react.has) put(&b, "%s{\"kind\":\"fast_react\",\"value\":%.2f}", first ? "" : ",", s->fast_react.value);
    put(&b, "]}");
    return done(&b);
}

int pp_session_summary(const pp_session_t *s, bool en, char *out, size_t size) {
    static const char *const ZH[PP_GAME_COUNT] = {"掷了 %u 次骰子", "转盘转了 %u 次", "抢拍比了 %u 场", "炸弹炸了 %u 次", "十秒挑战玩了 %u 次"};
    static const char *const EN[PP_GAME_COUNT] = {"rolled the dice %u times", "spun the wheel %u times", "played %u Snap matches",
                                                  "set off the bomb %u times", "tried Ten Seconds %u times"};
    buf_t b = {out, size, 0, false};
    if (size) out[0] = 0;
    int parts = 0;
    for (int i = 0; i < PP_GAME_COUNT; i++) {
        if (!s->rounds[i]) continue;
        if (parts) put(&b, en ? ", " : "，");
        put(&b, en ? EN[i] : ZH[i], s->rounds[i]);
        parts++;
    }
    if (!parts) put(&b, en ? "had a look at Party Play" : "看了看聚会玩");
    return done(&b);
}

int pp_session_stats(const pp_session_t *s, bool en, char value[3][16], char label[3][24]) {
    const int rounds = pp_session_total_rounds(s), minutes = minutes_of(s);
    int n = 0;
    if (!rounds) return 0;  // nothing played: Yay draws no ring, it only says the sentence
    snprintf(value[n], 16, "%d", rounds), snprintf(label[n], 24, "%s", en ? "rounds" : "局"), n++;
    if (s->players >= 2) snprintf(value[n], 16, "%d", s->players), snprintf(label[n], 24, "%s", en ? "players" : "人"), n++;
    if (minutes >= 1) snprintf(value[n], 16, "%d", minutes), snprintf(label[n], 24, "%s", en ? "min" : "分钟"), n++;
    return n;
}

int pp_session_data_json(const pp_session_t *s, char *out, size_t size) {
    buf_t b = {out, size, 0, false};
    if (size) out[0] = 0;
    put(&b, "{\"games\":");
    games_json(&b, s);
    if (s->players >= 1) put(&b, ",\"players\":%d", s->players);
    put(&b, "}");
    return done(&b);
}
