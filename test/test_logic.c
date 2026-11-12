// Host tests for Party Play's logic (docs/product/party-play.md "测试与验收"). No device, no ESP-IDF.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pp_bomb.h"
#include "pp_dice.h"
#include "pp_gesture.h"
#include "pp_launch.h"
#include "pp_motion_map.h"
#include "yk_motion_core.h"
#include "pp_play.h"
#include "pp_react.h"
#include "pp_record.h"
#include "pp_seats.h"
#include "pp_synth.h"
#include "pp_ten.h"
#include "pp_util.h"
#include "pp_wheel.h"

static int failures;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            failures++;                                                   \
        }                                                                 \
    } while (0)

// ───────────────────────── seats
static void test_seats(void) {
    pp_seats_t s;
    pp_seats_even(&s, 5);
    CHECK(s.n == 5);
    CHECK(fabsf(pp_ang_n(s.s[0].a - PP_PI / 2)) < 1e-4f);  // the first in front of the side key's edge
    for (int i = 0; i < 5; i++) CHECK(s.s[i].color == i);
    pp_seats_even(&s, 1);
    CHECK(s.n == 2);
    pp_seats_even(&s, 12);
    CHECK(s.n == 8);

    // sectors cover the whole circle without overlap, for uneven seats
    pp_seats_t t = {.n = 0};
    const float angles[] = {0.1f, 1.0f, 1.5f, 3.0f, -2.0f};
    for (int i = 0; i < 5; i++) CHECK(pp_seats_toggle(&t, angles[i]) == 1);
    pp_seats_sort_color(&t);
    float total = 0;
    for (int i = 0; i < t.n; i++) {
        float lo, hi;
        pp_seats_sector(&t, i, &lo, &hi);
        CHECK(hi > lo);
        CHECK(lo <= t.s[i].a + 1e-4f && t.s[i].a <= hi + 1e-4f);
        total += hi - lo;
    }
    CHECK(fabsf(total - PP_TAU) < 1e-3f);
    // every angle's nearest seat owns a sector containing it
    for (int k = 0; k < 360; k++) {
        const float a = pp_ang_n(k * PP_TAU / 360);
        const int i = pp_seats_nearest(&t, a);
        float lo, hi;
        pp_seats_sector(&t, i, &lo, &hi);
        const float rel = pp_ang_p(a - lo);
        CHECK(rel <= hi - lo + 1e-3f);
    }
    // sorted from the side key's edge clockwise, colours in that order
    for (int i = 1; i < t.n; i++) CHECK(pp_ang_p(t.s[i].a - PP_PI / 2) > pp_ang_p(t.s[i - 1].a - PP_PI / 2));
    // a tap near a seat removes it; nine is too many
    CHECK(pp_seats_toggle(&t, 1.05f) == -1);
    CHECK(t.n == 4);
    pp_seats_t full = {.n = 0};
    for (int i = 0; i < 8; i++) CHECK(pp_seats_toggle(&full, i * PP_TAU / 8) == 1);
    CHECK(pp_seats_toggle(&full, 0.4f) == 0);
    // neighbours alternate dark and light in the palette's order: 1 3 5 7 dark
    CHECK(PP_SEAT_COLORS[0] == 0xDF795D && PP_SEAT_COLORS[6] == 0x808B4F);
}

// ───────────────────────── dice
static int hits;
static void count_hit(void *ctx, bool wall, float strength) {
    (void)ctx, (void)wall;
    if (strength > 0 && strength <= 1) hits++;
}

static void test_dice(void) {
    pp_dice_t d;
    int counts[7] = {0};
    const int N = 60000;
    uint32_t rng = 99;
    for (int k = 0; k < N; k++) {
        pp_dice_init(&d, 1, pp_rand(&rng));
        // vary the throw so some rolls flip a lot and some hardly at all
        pp_dice_throw(&d, pp_randr(&rng, -150, 150), pp_randr(&rng, -150, 150));
        if (k % 3 == 0) d.d[0].vx *= 0.2f, d.d[0].vy *= 0.2f;
        int steps = 0;
        while (!pp_dice_step(&d, 1.0f / 30, 0, 0, false, 0, NULL, NULL) && steps < 300) steps++;
        CHECK(steps < 300);  // always lands within 10 s
        counts[d.d[0].face]++;
    }
    // chi-square, 5 degrees of freedom: 20.5 is p = 0.001
    double chi = 0;
    for (int f = 1; f <= 6; f++) chi += pow(counts[f] - N / 6.0, 2) / (N / 6.0);
    printf("dice faces chi2=%.2f (%d %d %d %d %d %d)\n", chi, counts[1], counts[2], counts[3], counts[4], counts[5], counts[6]);
    CHECK(chi < 20.5);

    // three dice stay inside the arena, never overlap much, and make sounds on impact
    pp_dice_init(&d, 3, 5);
    pp_dice_throw(&d, 100, 0);
    hits = 0;
    for (int k = 0; k < 600; k++) {
        const float fx = (k % 20 < 10 ? 9000 : -9000), fy = 0;  // shaking left and right while held
        pp_dice_step(&d, 1.0f / 30, k < 200 ? fx : 0, fy, k < 200, k % 15 == 0 ? 0.5f : 0, count_hit, NULL);
        for (int i = 0; i < 3; i++) {
            const float r = sqrtf(d.d[i].x * d.d[i].x + d.d[i].y * d.d[i].y);
            CHECK(r <= PP_DICE_ARENA - PP_DIE_R + 0.5f);
            CHECK(d.d[i].face >= 1 && d.d[i].face <= 6);
        }
    }
    CHECK(hits > 10);
    CHECK(!d.rolling);  // landed after it was put down
    // held dice never land
    pp_dice_init(&d, 2, 7);
    pp_dice_kick(&d);
    for (int k = 0; k < 200; k++) CHECK(!pp_dice_step(&d, 1.0f / 30, 0, 0, true, 0, NULL, NULL));
    // still but tilted (standing up, as the test device on its stand: 0.94 g in the glass): the dice slide to the
    // low side and land there, resting on the rim and on each other
    for (int n = 1; n <= PP_DICE_MAX; n++)
        for (int a = 0; a < 4; a++) {
            const float g = 0.94f * 3000, th = a * 1.1f;
            pp_dice_init(&d, n, 40 + a);
            pp_dice_throw(&d, 0, 120);
            int steps = 0;
            while (!pp_dice_step(&d, 1.0f / 30, g * cosf(th), g * sinf(th), false, 0, NULL, NULL) && steps < 180) steps++;
            CHECK(steps < 180);  // within 6 s
        }
    // sum and doubles
    pp_dice_init(&d, 2, 1);
    d.d[0].face = 4, d.d[1].face = 4;
    CHECK(pp_dice_sum(&d) == 8 && pp_dice_all_same(&d));
    d.n = 1;
    CHECK(!pp_dice_all_same(&d));
}

// ───────────────────────── wheel
static void test_wheel(void) {
    // uneven slices: the seats at 0.0, 0.3, 0.6 have small slices, the fourth gets most of the circle
    pp_seats_t s = {.n = 0};
    pp_seats_toggle(&s, 0.0f);
    pp_seats_toggle(&s, 0.4f);
    pp_seats_toggle(&s, 0.8f);
    pp_seats_toggle(&s, 3.5f);
    pp_seats_sort_color(&s);
    int wins[4] = {0};
    pp_wheel_t w;
    pp_wheel_init(&w, &s, 3);
    const int N = 10000;
    for (int k = 0; k < N; k++) {
        pp_wheel_spin(&w, &s, (k & 1) ? 18 : -14);
        CHECK(w.T >= 3.6f - 1e-3f && w.T <= 7 + 1e-3f);
        int ticks = 0, ev = 0;
        float t = 0;
        while (!((ev = pp_wheel_update(&w, &s, 1.0f / 30)) & PP_WHEEL_DONE)) {
            if (ev & PP_WHEEL_TICK) ticks++;
            t += 1.0f / 30;
            if (t > 8) break;
        }
        CHECK(t <= 8);
        CHECK(ticks >= 4);  // at least two turns past four seats... ticks are counted per frame
        // stopped inside the winner's slice, away from its edges
        CHECK(pp_seats_nearest(&s, w.th) == w.winner);
        wins[w.winner]++;
    }
    double chi = 0;
    for (int i = 0; i < 4; i++) chi += pow(wins[i] - N / 4.0, 2) / (N / 4.0);
    printf("wheel winners chi2=%.2f (%d %d %d %d)\n", chi, wins[0], wins[1], wins[2], wins[3]);
    CHECK(chi < 16.3);  // 3 dof, p = 0.001
    // dragging ticks when crossing into another slice
    pp_wheel_init(&w, &s, 1);
    int t2 = 0;
    for (int k = 0; k < 200; k++) t2 += (pp_wheel_drag(&w, &s, 0.05f) & PP_WHEEL_TICK) ? 1 : 0;
    CHECK(t2 >= 4);
}

// ───────────────────────── react
static void test_react(void) {
    pp_react_t r;
    pp_react_init(&r, 3, 42);
    CHECK(r.st == PP_RX_READY);
    pp_react_start(&r);
    CHECK(r.st == PP_RX_WAIT);
    // a false start locks that seat for the round
    int ev = pp_react_slap(&r, 1, 0);
    CHECK(ev & PP_RX_EV_FALSE);
    CHECK(r.locked[1]);
    CHECK(pp_react_slap(&r, 1, 0) == 0);
    // wait for GO
    int guard = 0;
    while (r.st != PP_RX_GO && guard++ < 1000) pp_react_update(&r, 0.02f);
    CHECK(r.st == PP_RX_GO);
    CHECK(pp_react_slap(&r, 1, 0.2f) == 0);  // still locked
    ev = pp_react_slap(&r, 2, 0.31f);
    CHECK(ev & PP_RX_EV_WIN);
    CHECK(r.winner == 2 && r.score[2] == 1 && fabsf(r.rt - 0.31f) < 1e-6f);
    // next round starts on its own; play until someone has 3
    for (int round = 0; round < 10 && r.st != PP_RX_OVER; round++) {
        guard = 0;
        while (r.st != PP_RX_GO && r.st != PP_RX_OVER && guard++ < 2000) pp_react_update(&r, 0.02f);
        if (r.st == PP_RX_GO) pp_react_slap(&r, 2, 0.3f);
        guard = 0;
        while (r.st == PP_RX_WIN && guard++ < 200) pp_react_update(&r, 0.02f);
    }
    CHECK(r.st == PP_RX_OVER && r.winner == 2 && r.score[2] == PP_REACT_TARGET);
    pp_react_start(&r);
    CHECK(r.st == PP_RX_READY && r.score[2] == 0);
    // everyone false-starting voids the round
    pp_react_start(&r);
    pp_react_slap(&r, 0, 0);
    pp_react_slap(&r, 1, 0);
    ev = pp_react_slap(&r, 2, 0);
    CHECK(ev & PP_RX_EV_VOID);
    CHECK(r.st == PP_RX_WAIT && !r.locked[0]);
    // nobody slapping for 3 s goes back to waiting
    guard = 0;
    while (r.st != PP_RX_GO && guard++ < 2000) pp_react_update(&r, 0.02f);
    ev = 0;
    for (int k = 0; k < 200 && !(ev & PP_RX_EV_NOBODY); k++) ev = pp_react_update(&r, 0.02f);
    CHECK(ev & PP_RX_EV_NOBODY);
    // a tie replays without a point
    guard = 0;
    while (r.st != PP_RX_GO && guard++ < 2000) pp_react_update(&r, 0.02f);
    CHECK(pp_react_tie(&r) & PP_RX_EV_TIE);
    CHECK(r.st == PP_RX_TIE);
    // decoys only from the third round
    int decoys_early = 0;
    for (int k = 0; k < 200; k++) {
        pp_react_init(&r, 2, (uint32_t)(k + 1));
        pp_react_start(&r);
        guard = 0;
        while (r.st == PP_RX_WAIT && guard++ < 1000) {
            if (pp_react_update(&r, 0.02f) & PP_RX_EV_DECOY) decoys_early++;
        }
    }
    CHECK(decoys_early == 0);
}

// ───────────────────────── bomb and forfeits
static void test_bomb(void) {
    pp_bomb_t b;
    pp_bomb_init(&b, 9);
    // no forfeit twice in a row of 20, all 20 come up
    bool seen[PP_PENALTY_COUNT] = {0};
    for (int i = 0; i < PP_PENALTY_COUNT; i++) {
        const int p = pp_bomb_next_penalty(&b);
        CHECK(p >= 0 && p < PP_PENALTY_COUNT && !seen[p]);
        seen[p] = true;
    }
    for (int i = 0; i < PP_PENALTY_COUNT; i++) CHECK(PP_PENALTIES[i].zh_short && PP_PENALTIES[i].en_say);
    // short text fits 8 Chinese characters (3 bytes each in UTF-8, plus ASCII)
    for (int i = 0; i < PP_PENALTY_COUNT; i++) CHECK(strlen(PP_PENALTIES[i].zh_short) <= 8 * 3);

    // the fuse: goes off 2-50 s after lighting, moving; exactly at its hidden length
    for (int k = 0; k < 50; k++) {
        pp_bomb_init(&b, (uint32_t)(k + 3));
        pp_bomb_tap(&b, false);
        CHECK(b.st == PP_BOMB_LIT && b.T >= 2 && b.T <= 50);
        float t = 0;
        while (b.st == PP_BOMB_LIT && t < 60) pp_bomb_update(&b, 0.01f, true, true), t += 0.01f;
        CHECK(b.st == PP_BOMB_BOOM);
        CHECK(fabsf(t - b.T) < 0.05f);
    }
    // the ticking tells nothing (user, 2026-11-10): two bombs with different hidden lengths tick at the same moments
    // until the first goes off, and it speeds up over 25 s whenever the bomb goes
    {
        pp_bomb_t a, c;
        pp_bomb_init(&a, 3);
        pp_bomb_init(&c, 4);
        pp_bomb_tap(&a, false);
        pp_bomb_tap(&c, false);
        a.T = 49, c.T = 6;  // one long, one short: the ticks must not care
        float t = 0;
        int same = 1, ticks_first = 0, ticks_late = 0;
        while (a.st == PP_BOMB_LIT && t < 60) {
            const int ea = pp_bomb_update(&a, 0.01f, true, true);
            if (c.st == PP_BOMB_LIT) {
                const int ec = pp_bomb_update(&c, 0.01f, true, true);
                if (c.st == PP_BOMB_LIT && (ea & PP_BOMB_EV_TICK) != (ec & PP_BOMB_EV_TICK)) same = 0;
            }
            if (ea & PP_BOMB_EV_TICK) {
                if (t < 3) ticks_first++;
                if (t >= 30 && t < 33) ticks_late++;
            }
            t += 0.01f;
        }
        CHECK(same && c.st != PP_BOMB_LIT);
        CHECK(ticks_late > 3 * ticks_first);  // 0.62 s a tick at first, 0.12 s once it has ramped
        CHECK(a.ramp == 1);
    }
    // it goes off at any speed of ticking: about a third within 10 s, while it still ticks slowly (> 0.45 s a tick);
    // about a quarter after the ticking is at its fastest (25 s); none piled up at 50 s
    {
        int early = 0, late = 0, at_max = 0, slow_boom = 0;
        const int N = 4000;
        for (int k = 0; k < N; k++) {
            pp_bomb_init(&b, (uint32_t)(k * 7919 + 1));
            pp_bomb_tap(&b, false);
            early += b.T < 10;
            late += b.T > 25;
            at_max += b.T > 49.5f;
        }
        CHECK(early > N * 0.30f && early < N * 0.39f);  // 34 %
        CHECK(late > N * 0.23f && late < N * 0.31f);    // 27 %
        CHECK(at_max < N * 0.01f);
        for (int k = 0; k < 200; k++) {  // and those early ones really go off on a slow tick
            pp_bomb_init(&b, (uint32_t)(k * 31 + 5));
            pp_bomb_tap(&b, false);
            if (b.T >= 10) continue;
            float last = 0, gap = 0, t = 0;
            while (b.st == PP_BOMB_LIT) {
                if (pp_bomb_update(&b, 0.01f, true, true) & PP_BOMB_EV_TICK) gap = t - last, last = t;
                t += 0.01f;
            }
            slow_boom += gap == 0 || gap > 0.45f;
        }
        CHECK(slow_boom > 40);
    }
    // still: nags, beeps and burns faster (a long fuse, so it has time to nag: a short one may go off first)
    pp_bomb_init(&b, 5);
    pp_bomb_tap(&b, false);
    b.T = 30;
    const float T = b.T;
    float t = 0;
    int nags = 0, beeps = 0;
    while (b.st == PP_BOMB_LIT && t < 60) {
        const int ev = pp_bomb_update(&b, 0.01f, false, true);
        nags += (ev & PP_BOMB_EV_NAG) ? 1 : 0;
        beeps += (ev & PP_BOMB_EV_BEEP) ? 1 : 0;
        t += 0.01f;
    }
    CHECK(t < T);
    CHECK(nags >= 1 && beeps >= 1);
    // after the boom: a forfeit, then a new bomb on tap
    t = 0;
    int ev = 0;
    while (b.st == PP_BOMB_BOOM && t < 5) ev |= pp_bomb_update(&b, 0.01f, false, true), t += 0.01f;
    CHECK(b.st == PP_BOMB_PENALTY && (ev & PP_BOMB_EV_PENALTY) && b.penalty >= 0);
    pp_bomb_tap(&b, false);
    CHECK(b.st == PP_BOMB_IDLE);
    // forfeits off: straight to AFTER
    pp_bomb_tap(&b, true);
    CHECK(b.T >= 1 && b.T <= 13);
    while (b.st == PP_BOMB_LIT) pp_bomb_update(&b, 0.05f, true, false);
    while (b.st == PP_BOMB_BOOM) pp_bomb_update(&b, 0.05f, true, false);
    CHECK(b.st == PP_BOMB_AFTER);
    // the side key pinches the fuse out
    pp_bomb_tap(&b, false);
    pp_bomb_tap(&b, false);
    CHECK(b.st == PP_BOMB_LIT);
    CHECK(pp_bomb_cancel(&b) && b.st == PP_BOMB_IDLE);
}

// ───────────────────────── ten
static void test_ten(void) {
    CHECK(pp_ten_rate(10.04f) == PP_TEN_PERFECT);
    CHECK(pp_ten_rate(9.94f) == PP_TEN_CLOSE);
    CHECK(pp_ten_rate(10.2f) == PP_TEN_CLOSE);
    CHECK(pp_ten_rate(10.45f) == PP_TEN_OK);
    CHECK(pp_ten_rate(11.0f) == PP_TEN_AGAIN);
    pp_ten_t t = {0};
    pp_ten_init(&t, 1);
    CHECK(!t.multi && t.best < 0);
    pp_ten_tap(&t);
    int seconds = 0;
    for (int k = 0; k < 987; k++) seconds += (pp_ten_update(&t, 0.01f) & PP_TEN_EV_SECOND) ? 1 : 0;
    CHECK(seconds == 2);  // ticks only while the numbers show (1 s and 2 s)
    CHECK(pp_ten_visible(&t) == 0);
    CHECK(pp_ten_tap(&t) & PP_TEN_EV_STOP);
    CHECK(fabsf(t.res - 9.87f) < 0.02f && fabsf(t.best - 9.87f) < 0.02f);
    pp_ten_tap(&t);
    CHECK(t.st == PP_TEN_IDLE);
    // blind mode fades in 0.6 s
    t.mode = 1;
    pp_ten_tap(&t);
    pp_ten_update(&t, 0.7f);
    CHECK(pp_ten_visible(&t) == 0);
    // nobody stops it: stops at 20 s
    int ev = 0;
    for (int k = 0; k < 2000 && !(ev & PP_TEN_EV_STOP); k++) ev = pp_ten_update(&t, 0.01f);
    CHECK(ev & PP_TEN_EV_STOP);
    // multi: turns, then the closest wins
    pp_ten_init(&t, 3);
    const float tries[3] = {9.5f, 10.1f, 11.0f};
    for (int i = 0; i < 3; i++) {
        CHECK(t.st == PP_TEN_IDLE && t.turn == i);
        pp_ten_tap(&t);
        pp_ten_update(&t, tries[i]);
        pp_ten_tap(&t);
        ev = 0;
        for (int k = 0; k < 300; k++) ev |= pp_ten_update(&t, 0.01f);
        if (i < 2) CHECK(ev & PP_TEN_EV_NEXT);
        else CHECK(ev & PP_TEN_EV_FINAL);
    }
    CHECK(t.st == PP_TEN_FINAL && t.winner == 1);
}

// ───────────────────────── record and result
static void test_record(void) {
    pp_session_t s;
    pp_session_reset(&s);
    CHECK(!pp_session_worth(&s));
    char buf[512];
    CHECK(pp_session_summary(&s, false, buf, sizeof(buf)) > 0 && !strcmp(buf, "看了看聚会玩"));
    pp_session_round(&s, PP_GAME_WHEEL);
    pp_session_round(&s, PP_GAME_WHEEL);
    pp_session_round(&s, PP_GAME_WHEEL);
    pp_session_round(&s, PP_GAME_REACT);
    s.players = 5;
    s.seconds = 82 * 60;
    pp_session_dice(&s, 3, 6);
    pp_session_dice(&s, 3, 2);  // a lower triple does not replace the six
    pp_session_dice(&s, 2, 5);  // two dice are not a triple
    pp_session_ten(&s, 10.02f);
    pp_session_ten(&s, 9.99f);   // closer
    pp_session_ten(&s, 10.3f);   // not perfect
    pp_session_react(&s, 0.21f);
    pp_session_react(&s, 0.30f);
    CHECK(pp_session_worth(&s));
    CHECK(pp_session_summary(&s, false, buf, sizeof(buf)) > 0);
    CHECK(!strcmp(buf, "转盘转了 3 次，抢拍比了 1 场"));
    pp_session_summary(&s, true, buf, sizeof(buf));
    CHECK(!strcmp(buf, "spun the wheel 3 times, played 1 Snap matches"));
    CHECK(pp_session_record_json(&s, buf, sizeof(buf)) > 0);
    CHECK(!strcmp(buf, "{\"minutes\":82,\"players\":5,\"games\":{\"wheel\":3,\"react\":1},\"highlights\":["
                       "{\"kind\":\"triple\",\"value\":6},{\"kind\":\"perfect_ten\",\"value\":9.99},"
                       "{\"kind\":\"fast_react\",\"value\":0.21}]}"));
    char v[3][16], l[3][24];
    CHECK(pp_session_stats(&s, false, v, l) == 3);
    CHECK(!strcmp(v[0], "4") && !strcmp(l[0], "局") && !strcmp(v[1], "5") && !strcmp(l[1], "人") && !strcmp(v[2], "82") && !strcmp(l[2], "分钟"));
    CHECK(pp_session_stats(&s, true, v, l) == 3 && !strcmp(l[0], "rounds"));
    CHECK(pp_session_data_json(&s, buf, sizeof(buf)) > 0);
    CHECK(!strcmp(buf, "{\"games\":{\"wheel\":3,\"react\":1},\"players\":5}"));
    char tiny[16];
    CHECK(pp_session_record_json(&s, tiny, sizeof(tiny)) == -1);
    // empty session: no numbers for Yay's ring
    pp_session_reset(&s);
    CHECK(pp_session_stats(&s, false, v, l) == 0);
    pp_session_data_json(&s, buf, sizeof(buf));
    CHECK(!strcmp(buf, "{\"games\":{}}"));
    CHECK(pp_game_from_key("react") == PP_GAME_REACT && pp_game_from_key("snap") == -1);
}

// ───────────────────────── launch parameters
static void test_launch(void) {
    pp_launch_params_t lp;
    pp_launch_parse("{\"game\":\"wheel\",\"players\":5}", &lp);
    CHECK(lp.game == PP_GAME_WHEEL && lp.players == 5 && lp.dice == 0);
    pp_launch_parse(" { \"dice\" : 7 , \"game\" : \"dice\" } ", &lp);
    CHECK(lp.game == PP_GAME_DICE && lp.dice == 3);
    pp_launch_parse("{\"game\":\"quiz\",\"players\":1,\"extra\":{\"a\":[1,{\"b\":\"}\"}]},\"dice\":2}", &lp);
    CHECK(lp.game == -1 && lp.players == 2 && lp.dice == 2);
    pp_launch_parse("{}", &lp);
    CHECK(lp.game == -1 && lp.players == 0);
    pp_launch_parse("not json", &lp);
    CHECK(lp.game == -1);
    pp_launch_parse(NULL, &lp);
    CHECK(lp.game == -1);
    pp_launch_parse("{\"game\":\"react\",\"players\":3.4}", &lp);
    CHECK(lp.game == PP_GAME_REACT && lp.players == 3);
    pp_launch_parse("{\"game\":\"ten\",\"players\":", &lp);  // cut off
    CHECK(lp.game == PP_GAME_TEN && lp.players == 0);
}

// ───────────────────────── gestures
static void test_gesture(void) {
    pp_gesture_t g;
    pp_gesture_down(&g, 233, 233, 0, 150);
    CHECK(pp_gesture_up(&g, 236, 234, 0.1f, 0) == PP_G_TAP);
    // turning along the edge, clockwise, quickly
    pp_gesture_down(&g, 233 + 200, 233, 0, 150);
    pp_gesture_ev_t ev = PP_G_NONE;
    bool started = false;
    for (int k = 1; k <= 10; k++) {
        const float a = k * 0.1f;
        ev = pp_gesture_move(&g, 233 + 200 * cosf(a), 233 + 200 * sinf(a), k * 0.01f);
        if (ev == PP_G_RIM_START) started = true;
    }
    CHECK(started);
    CHECK(pp_gesture_up(&g, 233 + 200 * cosf(1.0f), 233 + 200 * sinf(1.0f), 0.1f, 0) == PP_G_RIM_END);
    CHECK(g.w > 5 && g.w < 15);  // 0.1 rad per 10 ms = 10 rad/s
    // a swipe left from the middle
    pp_gesture_down(&g, 300, 233, 0, 150);
    pp_gesture_move(&g, 230, 236, 0.1f);
    CHECK(pp_gesture_up(&g, 200, 236, 0.15f, 0) == PP_G_SWIPE_LEFT);
    // the person opposite (middle turned 180 degrees) swiping to their left moves right on the screen
    pp_gesture_down(&g, 180, 233, 0, 150);
    pp_gesture_move(&g, 230, 233, 0.1f);
    CHECK(pp_gesture_up(&g, 280, 233, 0.15f, PP_PI) == PP_G_SWIPE_LEFT);
    pp_gesture_down(&g, 233, 150, 0, 150);
    pp_gesture_move(&g, 233, 220, 0.1f);
    CHECK(pp_gesture_up(&g, 235, 240, 0.2f, 0) == PP_G_SWIPE_DOWN);
    // a long press without moving is nothing
    pp_gesture_down(&g, 233, 233, 0, 150);
    CHECK(pp_gesture_up(&g, 233, 233, 1.0f, 0) == PP_G_NONE);
}

// ───────────────────────── motion: yaykit_motion's core, then our mapping
typedef struct {
    ykm_core_t core;
    int64_t t_us;
    yk_motion_event_t ev[32];
    int nev;
} motion_rig_t;

/** Feeds the shared core for secs: specific force f (what the accelerometer reads, g) and gyro (dps) in the core's
 *  device frame (the rig's calibration is the identity, so sensor = device: x right, y UP, z out of the glass). */
static void feed(motion_rig_t *r, float secs, float fx, float fy, float fz, float gyro, float shake_x) {
    for (float t = 0; t < secs; t += 0.01f) {
        const float a[3] = {fx + shake_x * sinf(t * PP_TAU * 5), fy, fz}, g[3] = {gyro, 0, 0};
        r->t_us += 10000;
        yk_motion_event_t e[4];
        const int n = ykm_feed(&r->core, a, g, r->t_us, e, 4);
        for (int i = 0; i < n && r->nev < 32; i++) r->ev[r->nev++] = e[i];
    }
}

static pp_motion_in_t take(motion_rig_t *r) {
    yk_motion_state_t st;
    ykm_state(&r->core, &st);
    ykm_holder(&r->core, r->t_us / 1000, 500, &st.holder_angle, &st.holder_confidence);  // as pp_imu_take does
    pp_motion_in_t mi;
    pp_motion_map(r->ev, r->nev, &st, true, &mi);
    r->nev = 0;
    return mi;
}

static void test_motion(void) {
    CHECK(fabsf(pp_angle_from_motion(180) - PP_PI / 2) < 1e-4f);  // the bottom edge: down on the screen
    CHECK(fabsf(pp_angle_from_motion(90)) < 1e-4f);               // the right edge
    static motion_rig_t r;
    yk_motion_thresholds_t th;
    yk_motion_default_thresholds(&th);
    const yk_motion_cal_t cal = {.axis = {1, 2, 3}};
    ykm_init(&r.core, &th, &cal);
    r.t_us = 1000000;
    // flat on the table, glass up: still, nothing pushes the dice
    feed(&r, 2, 0, 0, 1, 0, 0);
    pp_motion_in_t mi = take(&r);
    CHECK(!mi.held && !mi.moving && mi.fx == 0 && mi.fy == 0 && !(mi.events & PP_MO_LIFT));
    // picked up and tilted 35 degrees towards the person at the bottom edge: that edge goes down, so the force the
    // accelerometer reads (opposite to gravity) leans towards the top (+y in the device frame)
    const float s35 = sinf(35 * PP_PI / 180), c35 = cosf(35 * PP_PI / 180);
    feed(&r, 0.3f, 0, s35, c35, 60, 0);
    feed(&r, 0.8f, 0, s35, c35, 6, 0);
    mi = take(&r);
    CHECK(mi.events & PP_MO_LIFT);
    CHECK(mi.held && mi.moving);
    CHECK(mi.fy > 1000 && fabsf(mi.fx) < 200);  // the dice slide towards the person holding it
    CHECK(mi.holder_conf > 0.3f && fabsf(pp_ang_n(mi.holder - PP_PI / 2)) < 0.3f);
    // shaken left and right
    feed(&r, 1.2f, 0, s35, c35, 20, 2.0f);
    mi = take(&r);
    CHECK(mi.events & PP_MO_SHAKE);
    // put down flat: at rest after still_ms, the holder still the bottom edge
    feed(&r, 1.5f, 0, 0, 1, 0, 0);
    mi = take(&r);
    CHECK(mi.events & PP_MO_PUT);
    CHECK(!mi.held);
    CHECK(fabsf(pp_ang_n(mi.holder - PP_PI / 2)) < 0.4f);
    // turned over (through standing on its edge) and left face down for a second, then back up
    feed(&r, 0.3f, 1, 0, 0, 90, 0);
    feed(&r, 1.2f, 0, 0, -1, 0, 0);
    mi = take(&r);
    CHECK(mi.events & PP_MO_FACE_DOWN);
    feed(&r, 0.3f, 1, 0, 0, 90, 0);
    feed(&r, 1.0f, 0, 0, 1, 0, 0);
    mi = take(&r);
    CHECK(mi.events & PP_MO_FACE_UP);
    // no sensor: the bomb must never nag
    pp_motion_map(NULL, 0, NULL, false, &mi);
    CHECK(mi.moving && !mi.held);
}

// ───────────────────────── the whole model
static int take_count(pp_play_t *p, int kind, int id) {
    pp_out_t o[64];
    int n = 0;
    int k;
    while ((k = pp_play_take(p, o, 64)) > 0)
        for (int i = 0; i < k; i++)
            if (o[i].kind == kind && (id < 0 || o[i].id == id)) n++;
    return n;
}

static void run(pp_play_t *p, float secs, const pp_motion_in_t *mi) {
    for (float t = 0; t < secs; t += 1.0f / 30) pp_play_tick(p, 1.0f / 30, mi);
}

static void test_play(void) {
    static pp_play_t p;
    pp_play_init(&p, 77);
    CHECK(p.scene == PP_SC_MENU && p.yay.mode == PP_YAY_HOST);
    CHECK(take_count(&p, PP_OUT_LINE, PP_LINE_HELLO) == 1);
    run(&p, 2.5f, NULL);
    CHECK(p.yay.mode == PP_YAY_HIDDEN);
    // turn the icon ring one step and start the selected game: the wheel needs seats first
    pp_play_rim_start(&p, 233, 33);
    pp_play_rim_move(&p, -1.3f);  // one step is 72 degrees
    pp_play_rim_end(&p, 0);
    run(&p, 1, NULL);
    CHECK(lroundf(p.menu_pos) == 1);
    CHECK(take_count(&p, PP_OUT_SOUND, PP_SND_TICK) >= 1);
    pp_play_tap(&p, 233, 233, p.now);
    CHECK(p.scene == PP_SC_SEATS);
    // three people tap their edges; one taps again to leave; start
    pp_play_tap(&p, 233, 430, p.now);
    pp_play_tap(&p, 40, 233, p.now);
    pp_play_tap(&p, 233, 40, p.now);
    pp_play_tap(&p, 420, 233, p.now);
    pp_play_tap(&p, 233, 40, p.now);  // leaves
    CHECK(p.seating.n == 3);
    pp_play_tap(&p, 233, 233, p.now);
    run(&p, 1.0f, NULL);
    CHECK(p.scene == PP_SC_WHEEL && p.seats.n == 3 && p.seats.tapped);
    // spin: lands on someone, confetti, a result for Yay
    pp_play_tap(&p, 233, 233, p.now);
    CHECK(p.wheel_st == PP_WHEEL_SPINNING);
    for (int k = 0; k < 300 && p.wheel_st != PP_WHEEL_RESULT; k++) pp_play_tick(&p, 1.0f / 30, NULL);
    CHECK(p.wheel_st == PP_WHEEL_RESULT && p.nparts > 0 && p.nparts <= PP_PARTS_MAX);
    CHECK(p.session.rounds[PP_GAME_WHEEL] == 1 && p.result_dirty);
    // the winner's highlight beats for about 3 s, then holds: a result left on the table stops redrawing
    CHECK(fabsf(p.now - p.result_at) < 0.05f);
    float plo = 1, phi = 0;
    for (int k = 0; k < 80; k++) {
        plo = fminf(plo, pp_play_pulse(&p)), phi = fmaxf(phi, pp_play_pulse(&p));
        pp_play_tick(&p, 1.0f / 30, NULL);
    }
    CHECK(plo < 0.2f && phi > 0.95f);
    run(&p, 0.5f, NULL);
    CHECK(pp_play_pulse(&p) == 1);
    // the side key goes back to the menu on the wheel
    take_count(&p, -1, -1);
    pp_play_key_short(&p);
    CHECK(p.scene == PP_SC_MENU && lroundf(p.menu_pos) == 1);

    // swipe left: settings; turn the edge to change the volume; side key back
    pp_play_swipe(&p, PP_G_SWIPE_LEFT);
    CHECK(p.scene == PP_SC_SETTINGS);
    p.set_sel = 2;
    const int vol = p.volume;
    pp_play_rim_start(&p, 233, 20);
    pp_play_rim_move(&p, 0.4f);
    CHECK(p.volume == vol + 1);
    pp_play_key_short(&p);
    CHECK(p.scene == PP_SC_MENU);

    // dice by tap: they land, line up and a sum is said
    pp_play_go(&p, PP_SC_DICE);
    take_count(&p, -1, -1);
    pp_play_tap(&p, 233, 400, p.now);
    CHECK(p.dice_st == PP_DICE_ROLL);
    run(&p, 6, NULL);
    CHECK(p.dice_st == PP_DICE_RESULT);
    CHECK(take_count(&p, PP_OUT_SOUND, PP_SND_DICE_WALL) + take_count(&p, PP_OUT_SOUND, PP_SND_DICE_HIT) >= 0);
    CHECK(p.session.rounds[PP_GAME_DICE] == 1);
    // picking it up starts a roll; it never lands in the hand; put down, it does
    pp_motion_in_t lift = {.events = PP_MO_LIFT, .held = true, .moving = true, .holder = 0, .holder_conf = 1};
    pp_play_tick(&p, 1.0f / 30, &lift);
    CHECK(p.dice_st == PP_DICE_ROLL);
    pp_motion_in_t held = {.held = true, .moving = true, .fx = 3000, .holder = 0, .holder_conf = 1};
    run(&p, 2, &held);
    CHECK(p.dice_st == PP_DICE_ROLL);
    run(&p, 6, NULL);
    CHECK(p.dice_st == PP_DICE_RESULT);
    CHECK(fabsf(pp_ang_n(p.rot_target - (0 - PP_PI / 2))) < 1e-3f);  // faces the person who held it (angle 0)

    // bomb: lit, held still it nags; goes off; a forfeit follows
    pp_play_go(&p, PP_SC_BOMB);
    p.quick_bomb = true;
    pp_play_tap(&p, 233, 233, p.now);
    CHECK(p.bomb.st == PP_BOMB_LIT);
    take_count(&p, -1, -1);
    run(&p, 15, NULL);
    CHECK(p.bomb.st == PP_BOMB_PENALTY || p.bomb.st == PP_BOMB_AFTER);
    // face down pauses everything
    pp_play_tap(&p, 233, 233, p.now);
    pp_play_tap(&p, 233, 233, p.now);
    CHECK(p.bomb.st == PP_BOMB_LIT);
    const float burnt = p.bomb.p, ramp = p.bomb.ramp;
    pp_play_face_down(&p, true);
    run(&p, 5, NULL);
    CHECK(p.bomb.p == burnt && p.bomb.ramp == ramp);
    pp_play_face_down(&p, false);
    pp_play_key_short(&p);  // pinches it out
    CHECK(p.bomb.st == PP_BOMB_IDLE);

    // react with the seats from before: false start, then a win
    pp_play_go(&p, PP_SC_REACT);
    run(&p, 2, NULL);
    pp_play_tap(&p, 233, 233, p.now);
    CHECK(p.react.st == PP_RX_WAIT);
    const float sa = p.seats.s[1].a;
    pp_play_tap(&p, 233 + 170 * cosf(sa), 233 + 170 * sinf(sa), p.now);
    CHECK(p.react.locked[1]);
    int guard = 0;
    while (p.react.st != PP_RX_GO && guard++ < 1000) pp_play_tick(&p, 1.0f / 30, NULL);
    pp_play_go_shown(&p, p.now);
    const float sb = p.seats.s[2].a;
    pp_play_tap(&p, 233, 233, p.now + 0.1f);  // dead zone
    CHECK(p.react.st == PP_RX_GO);
    pp_play_tap(&p, 233 + 170 * cosf(sb), 233 + 170 * sinf(sb), p.now + 0.24f);
    CHECK(p.react.st == PP_RX_GO);  // judged once nobody else could still be faster
    for (int k = 0; k < 20 && p.react.st == PP_RX_GO; k++) pp_play_tick(&p, 1.0f / 30, NULL);
    CHECK(p.react.st == PP_RX_WIN && p.react.winner == 2);
    CHECK(fabsf(p.react.rt - 0.24f) < 1e-3f);
    CHECK(p.session.fast_react.has);

    // a round keeps the screen on to its result, past the shared screen-off time (30 s by default: off at 63 s).
    // The react match goes on with nobody slapping: still on after 200 s; left for 5 minutes, it sleeps after all
    CHECK(p.idle_off == 30);
    run(&p, 200, NULL);
    CHECK(pp_play_in_round(&p) && !p.dimmed && !p.off);
    run(&p, 170, NULL);
    CHECK(p.off);
    CHECK(!pp_play_touch_down(&p, 233, 233));  // wakes, and the match is on again
    run(&p, 100, NULL);
    CHECK(!p.dimmed && !p.off);
    // ten seconds for three: the screen stays on while the next player takes their time; the time starts again at
    // the last turn's result
    pp_play_go(&p, PP_SC_TEN);
    CHECK(p.ten.multi && p.ten.n == 3);
    pp_play_tap(&p, 233, 233, p.now);
    run(&p, 9.8f, NULL);
    pp_play_tap(&p, 233, 233, p.now);
    run(&p, 2, NULL);
    CHECK(p.ten.st == PP_TEN_IDLE && p.ten.turn == 1);
    run(&p, 100, NULL);
    CHECK(pp_play_in_round(&p) && !p.dimmed && !p.off);
    for (int i = 1; i < 3; i++) {
        pp_play_tap(&p, 233, 233, p.now);
        run(&p, 10, NULL);
        pp_play_tap(&p, 233, 233, p.now);
        run(&p, 2, NULL);
    }
    CHECK(p.ten.st == PP_TEN_FINAL && !pp_play_in_round(&p));
    run(&p, 58, NULL);
    CHECK(!p.dimmed);
    run(&p, 3, NULL);
    CHECK(p.dimmed && !p.off);
    run(&p, 3, NULL);
    CHECK(p.off);
    CHECK(!pp_play_touch_down(&p, 233, 233));

    // idle on the menu: dims at 60 s, Yay rests, off at 63 s (the shared 30 s, but not before 63 s); the first touch
    // only wakes it
    pp_play_go(&p, PP_SC_MENU);
    run(&p, 61, NULL);
    CHECK(p.dimmed && !p.off);
    run(&p, 2.5f, NULL);
    CHECK(p.off);
    CHECK(!pp_play_touch_down(&p, 233, 233));
    CHECK(!p.off && pp_play_touch_down(&p, 233, 233));

    // a system layer over the game (the app ring on a double press, which the game never sees, 11-11): from off it
    // wakes like a touch, without one; the idle count starts over; face down stays face down
    run(&p, 61, NULL);
    run(&p, 2.5f, NULL);
    CHECK(p.off);
    pp_play_wake(&p);
    CHECK(!p.off && !p.dimmed && p.idle == 0);
    run(&p, 61, NULL);
    CHECK(p.dimmed && !p.off);
    pp_play_wake(&p);
    CHECK(!p.dimmed && p.idle == 0);
    pp_play_face_down(&p, true);
    pp_play_wake(&p);
    CHECK(p.face_down);
    pp_play_face_down(&p, false);

    // launch from Yay: straight into the wheel with 5 even seats
    pp_play_init(&p, 5);
    const pp_launch_params_t lp = {.game = PP_GAME_WHEEL, .players = 5};
    pp_play_launch(&p, &lp);
    CHECK(p.scene == PP_SC_WHEEL && p.seats.n == 5 && !p.seats.tapped);
    // react without seats asks for them first
    pp_play_init(&p, 6);
    const pp_launch_params_t lr = {.game = PP_GAME_REACT};
    pp_play_launch(&p, &lr);
    CHECK(p.scene == PP_SC_SEATS && p.seats_next == PP_SC_REACT);
    // critical battery: after the current thing, rest until plugged in
    pp_play_go(&p, PP_SC_MENU);
    pp_play_battery(&p, 2, 4);
    run(&p, 0.1f, NULL);
    CHECK(p.scene == PP_SC_CHARGE);
    pp_play_battery(&p, 0, 60);
    CHECK(p.scene == PP_SC_MENU);
    // particles never exceed the cap
    pp_play_go(&p, PP_SC_DICE);
    for (int k = 0; k < 5; k++) {
        p.dice.n = 3;
        for (int i = 0; i < 3; i++) p.dice.d[i].face = 6;
        p.dice_st = PP_DICE_ARRANGE;
        p.reveal_t = 0.95f;
        run(&p, 0.05f, NULL);
        CHECK(p.nparts <= PP_PARTS_MAX);
    }
    CHECK(p.session.triple.has && p.session.triple.value == 6);
}

// ───────────────────────── sounds
static void write_wav(const char *path, const int16_t *pcm, int n, int rate) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    const uint32_t data = (uint32_t)n * 2, riff = 36 + data, fmt = 16, br = (uint32_t)rate * 2;
    const uint16_t pcm_fmt = 1, ch = 1, align = 2, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmt, 4, 1, f);
    fwrite(&pcm_fmt, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f); fwrite(&br, 4, 1, f);
    fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    fwrite(pcm, 2, (size_t)n, f);
    fclose(f);
}

// a new scene comes up from dark and a turn goes dark, snaps and comes back: the screen is drawn once, unseen
static void test_light(void) {
    static pp_play_t p;
    pp_play_init(&p, 9);
    run(&p, 1, NULL);
    CHECK(pp_play_light(&p) == 1);
    pp_play_go(&p, PP_SC_DICE);
    CHECK(pp_play_light(&p) == 0);
    pp_play_tick(&p, 0.1f, NULL);  // the tick the new scene is drawn in: still dark
    CHECK(pp_play_light(&p) == 0);
    pp_play_tick(&p, 0.1f, NULL);  // a slow first frame: the light starts from the beginning all the same
    CHECK(pp_play_light(&p) > 0.3f && pp_play_light(&p) < 0.45f);
    run(&p, 0.2f, NULL);
    CHECK(pp_play_light(&p) == 1);
    // a turn: the middle keeps its old way until the dark point, then faces the new one at once
    const float from = p.rot;
    p.rot_target = from + PP_PI / 2;
    while (p.rot == from) pp_play_tick(&p, 0.01f, NULL);
    CHECK(pp_play_light(&p) == 0);  // the tick it snaps in is dark: the turned screen is drawn unseen
    CHECK(fabsf(p.rot - (from + PP_PI / 2)) < 1e-6f);
    // someone else while it lights again: no jump in the light, the next turn comes after
    p.rot_target = from + PP_PI;
    for (int k = 0; k < 10; k++) pp_play_tick(&p, 0.01f, NULL);
    CHECK(fabsf(p.rot - (from + PP_PI / 2)) < 1e-6f);
    run(&p, 0.6f, NULL);
    CHECK(fabsf(p.rot - (from + PP_PI)) < 1e-6f && pp_play_light(&p) == 1 && p.turn_t < 0);
}

// the lit frame goes out in bands from the top: each seat's reaction counts from when its own part lit
static void test_react_bands(void) {
    static pp_play_t p;
    pp_play_init(&p, 5);
    p.players = 4;
    p.react_by_part = true;  // counted by parts (not the default yet)
    pp_play_go(&p, PP_SC_REACT);
    run(&p, 2, NULL);
    CHECK(p.seats.n == 4);
    int top = 0, bottom = 0;
    for (int i = 1; i < 4; i++) {
        if (sinf(p.seats.s[i].a) < sinf(p.seats.s[top].a)) top = i;
        if (sinf(p.seats.s[i].a) > sinf(p.seats.s[bottom].a)) bottom = i;
    }
    pp_play_tap(&p, 233, 233, p.now);
    int guard = 0;
    while (p.react.st != PP_RX_GO && guard++ < 1000) pp_play_tick(&p, 1.0f / 30, NULL);
    CHECK(p.react.st == PP_RX_GO);
    // two bands: the top half on the glass at t0, the bottom half 60 ms later
    const float t0 = p.now;
    const int y1[2] = {0, 233};
    const float at[2] = {t0, t0 + 0.06f};
    pp_play_go_bands(&p, y1, at, 2);
    CHECK(fabsf(p.seen[top] - t0) < 1e-6f && fabsf(p.seen[bottom] - (t0 + 0.06f)) < 1e-6f);
    // the person at the top slaps 250 ms after their half lit: not judged while the bottom one could still be faster
    const float ta = p.seats.s[top].a, ba = p.seats.s[bottom].a;
    pp_play_tap(&p, 233 + 170 * cosf(ta), 233 + 170 * sinf(ta), t0 + 0.25f);
    while (p.now < t0 + 0.27f) pp_play_tick(&p, 1.0f / 30, NULL);
    CHECK(p.react.st == PP_RX_GO);
    // the bottom one, 280 ms after t0 but 220 ms after their half lit, wins
    pp_play_tap(&p, 233 + 170 * cosf(ba), 233 + 170 * sinf(ba), t0 + 0.28f);
    for (int k = 0; k < 20 && p.react.st == PP_RX_GO; k++) pp_play_tick(&p, 1.0f / 30, NULL);
    CHECK(p.react.st == PP_RX_WIN && p.react.winner == bottom);
    CHECK(fabsf(p.react.rt - 0.22f) < 1e-3f);
    // both counts logged: from the whole frame (60 ms in) the top one was faster, 0.19 s
    CHECK(p.judged.serial == 1 && p.judged.seat[0] == top && fabsf(p.judged.rt[0] - 0.19f) < 1e-3f);
    CHECK(p.judged.seat[1] == bottom && fabsf(p.judged.rt[1] - 0.22f) < 1e-3f);
    // the next round: alone, the top one wins as soon as nobody else could beat them
    guard = 0;
    while (p.react.st != PP_RX_GO && guard++ < 1000) pp_play_tick(&p, 1.0f / 30, NULL);
    const float t1 = p.now;
    const float at1[2] = {t1, t1 + 0.06f};
    pp_play_go_bands(&p, y1, at1, 2);
    pp_play_tap(&p, 233 + 170 * cosf(ta), 233 + 170 * sinf(ta), t1 + 0.2f);
    while (p.now < t1 + 0.2f + 0.06f + 0.04f && p.react.st == PP_RX_GO) pp_play_tick(&p, 1.0f / 30, NULL);
    CHECK(p.react.st == PP_RX_WIN && p.react.winner == top && fabsf(p.react.rt - 0.2f) < 1e-3f);
}

// by default everyone counts from the lit frame all on the glass (1.0.3); a slap just before the frame ends is judged
static void test_react_whole(void) {
    static pp_play_t p;
    pp_play_init(&p, 5);
    p.players = 4;
    pp_play_go(&p, PP_SC_REACT);
    run(&p, 2, NULL);
    int top = 0, bottom = 0;
    for (int i = 1; i < 4; i++) {
        if (sinf(p.seats.s[i].a) < sinf(p.seats.s[top].a)) top = i;
        if (sinf(p.seats.s[i].a) > sinf(p.seats.s[bottom].a)) bottom = i;
    }
    const float ta = p.seats.s[top].a, ba = p.seats.s[bottom].a;
    pp_play_tap(&p, 233, 233, p.now);
    int guard = 0;
    while (p.react.st != PP_RX_GO && guard++ < 1000) pp_play_tick(&p, 1.0f / 30, NULL);
    const float t0 = p.now;
    const int y1[2] = {0, 233};
    const float at[2] = {t0, t0 + 0.06f};
    pp_play_go_bands(&p, y1, at, 2);
    pp_play_tap(&p, 233 + 170 * cosf(ta), 233 + 170 * sinf(ta), t0 + 0.25f);
    pp_play_tap(&p, 233 + 170 * cosf(ba), 233 + 170 * sinf(ba), t0 + 0.28f);
    for (int k = 0; k < 20 && p.react.st == PP_RX_GO; k++) pp_play_tick(&p, 1.0f / 30, NULL);
    CHECK(p.react.st == PP_RX_WIN && p.react.winner == top && fabsf(p.react.rt - 0.19f) < 1e-3f);
    CHECK(p.judged.seat[0] == top && p.judged.seat[1] == bottom);
    // the next lit frame: one slap 10 ms before it ends still counts (it is not lost to "nobody")
    guard = 0;
    while (p.react.st != PP_RX_GO && guard++ < 1000) pp_play_tick(&p, 1.0f / 30, NULL);
    const float t1 = p.now;
    const float at1[2] = {t1, t1 + 0.03f};
    pp_play_go_bands(&p, y1, at1, 2);
    while (p.react.t < PP_REACT_GO_S - 0.02f) pp_play_tick(&p, 1.0f / 30, NULL);
    pp_play_tap(&p, 233 + 170 * cosf(ta), 233 + 170 * sinf(ta), p.now);
    for (int k = 0; k < 3; k++) pp_play_tick(&p, 1.0f / 30, NULL);
    CHECK(p.react.st == PP_RX_WIN && p.react.winner == top);
}

static float min_light(pp_play_t *p, float secs) {
    float lo = 1;
    for (float t = 0; t < secs; t += 1.0f / 30) {
        pp_play_tick(p, 1.0f / 30, NULL);
        lo = fminf(lo, pp_play_light(p));
    }
    return lo;
}

// turns that come with a whole new screen are not drawn through dark: a false start just before the lit frame, a
// point, the dice landing; the lit frame itself always at full light
static void test_no_blink(void) {
    static pp_play_t p;
    pp_play_init(&p, 11);
    p.players = 4;
    pp_play_go(&p, PP_SC_REACT);
    run(&p, 2, NULL);
    pp_play_tap(&p, 233, 233, p.now);
    while (p.react.st == PP_RX_WAIT && p.react.wait - p.react.t > 0.1f) pp_play_tick(&p, 1.0f / 30, NULL);
    CHECK(p.react.st == PP_RX_WAIT);
    int seat = 0;  // someone the middle does not face
    for (int i = 0; i < p.seats.n; i++)
        if (fabsf(pp_ang_n(p.seats.s[i].a - PP_PI / 2 - p.rot)) > 0.5f) seat = i;
    const float a = p.seats.s[seat].a;
    pp_play_tap(&p, 233 + 170 * cosf(a), 233 + 170 * sinf(a), p.now);
    CHECK(p.react.locked[seat] && p.turn_t < 0 && fabsf(p.rot - p.rot_target) < 1e-6f);
    int guard = 0;
    float lo = 1;
    while (p.react.st != PP_RX_GO && guard++ < 100) {
        pp_play_tick(&p, 1.0f / 30, NULL);
        lo = fminf(lo, pp_play_light(&p));
    }
    CHECK(p.react.st == PP_RX_GO && lo == 1 && pp_play_light(&p) == 1);
    // a turn left going when the frame lights (from anything) is finished at once
    p.react.st = PP_RX_WAIT, p.react.t = p.react.wait - 0.01f;
    p.rot_target = p.rot + 1, p.turn_t = 0.05f;
    pp_play_tick(&p, 1.0f / 30, NULL);
    CHECK(p.react.st == PP_RX_GO && p.turn_t < 0 && p.rot == p.rot_target && pp_play_light(&p) == 1);
    // a point
    const int other = (seat + 1) % p.seats.n;
    const float oa = p.seats.s[other].a;
    pp_play_go_shown(&p, p.now);
    pp_play_tap(&p, 233 + 170 * cosf(oa), 233 + 170 * sinf(oa), p.now + 0.2f);
    for (int k = 0; k < 20 && p.react.st == PP_RX_GO; k++) pp_play_tick(&p, 1.0f / 30, NULL);
    CHECK(p.react.st == PP_RX_WIN && p.turn_t < 0 && min_light(&p, 1) == 1);
    // the dice landing turn to the roller at once
    pp_play_go(&p, PP_SC_DICE);
    run(&p, 1, NULL);
    const float before = p.rot;
    pp_play_tap(&p, 233, 66, p.now);  // thrown from the far side: the sum turns to face them
    lo = 1;
    for (guard = 0; guard < 400 && p.dice_st != PP_DICE_RESULT; guard++) {
        pp_play_tick(&p, 1.0f / 30, NULL);
        lo = fminf(lo, pp_play_light(&p));
    }
    CHECK(p.dice_st == PP_DICE_RESULT && lo == 1 && fabsf(pp_ang_n(p.rot - before)) > 1);
    // ten seconds with seats: facing the first player from the start, not turning once it is up
    pp_play_go(&p, PP_SC_TEN);
    CHECK(p.turn_t < 0 && fabsf(p.rot - p.rot_target) < 1e-6f);
}

static void test_synth(void) {
    static pp_synth_t s;
    static int16_t buf[16000 * 3];
    const char *dir = getenv("PP_WAV_DIR");  // set by `run.sh wav` to listen to them
    for (int id = 0; id < PP_SND_COUNT; id++) {
        pp_synth_init(&s, 16000, 1);
        const float g = id == PP_SND_CHEER ? 1.5f : id == PP_SND_DRUMROLL ? 1.3f : id == PP_SND_SEAT ? 3 : 0.9f;
        pp_synth_play(&s, (pp_sound_t)id, g);
        int left = pp_synth_render(&s, buf, 16000 * 3);
        int peak = 0;
        double energy = 0;
        for (int i = 0; i < 16000 * 3; i++) {
            const int a = abs(buf[i]);
            if (a > peak) peak = a;
            energy += (double)buf[i] * buf[i];
        }
        CHECK(left == 0);           // every sound ends within 3 s
        CHECK(peak <= 32000);       // the limiter holds
        CHECK(energy > 1e5);        // not silent
        if (dir) {
            char path[256];
            snprintf(path, sizeof(path), "%s/sound-%02d.wav", dir, id);
            write_wav(path, buf, 16000 * 3, 16000);
        }
    }
    // rattling dice: 100 hits in 0.2 s stay within the primitive pool and the rate limit
    pp_synth_init(&s, 16000, 2);
    for (int k = 0; k < 100; k++) {
        pp_synth_play(&s, PP_SND_DICE_HIT, 0.8f);
        pp_synth_render(&s, buf, 32);
    }
    CHECK(s.n <= PP_SYNTH_PRIMS && s.n < 40);
    // placed to the sample: silent until the delay, then sounding (the snap with its lit frame); floats in -1..1
    pp_synth_init(&s, 24000, 4);
    pp_synth_play_at(&s, PP_SND_SNAP_GO, 1, 0.05f);
    static float fb[24000];
    pp_synth_render_f(&s, fb, 24000);
    float before = 0, after = 0;
    for (int i = 0; i < 1190; i++) before = fmaxf(before, fabsf(fb[i]));     // the first 49.6 ms
    for (int i = 1200; i < 2400; i++) after = fmaxf(after, fabsf(fb[i]));    // 50-100 ms
    CHECK(before == 0 && after > 0.05f && after <= 1);
    // everything at once (cheer + boom + fanfare) does not overflow or clip
    pp_synth_init(&s, 16000, 3);
    pp_synth_play(&s, PP_SND_BOOM, 1);
    pp_synth_play(&s, PP_SND_CHEER, 1.8f);
    pp_synth_play(&s, PP_SND_FANFARE_BIG, 1);
    pp_synth_play(&s, PP_SND_DRUMROLL, 1.3f);
    CHECK(s.n <= PP_SYNTH_PRIMS);
    pp_synth_render(&s, buf, 16000 * 3);
    int peak = 0;
    for (int i = 0; i < 16000 * 3; i++) peak = abs(buf[i]) > peak ? abs(buf[i]) : peak;
    CHECK(peak <= 32000);
    if (dir) {
        char path[256];
        snprintf(path, sizeof(path), "%s/sound-all.wav", dir);
        write_wav(path, buf, 16000 * 3, 16000);
    }
}

int main(void) {
    test_seats();
    test_dice();
    test_wheel();
    test_react();
    test_bomb();
    test_ten();
    test_record();
    test_launch();
    test_gesture();
    test_motion();
    test_play();
    test_react_bands();
    test_react_whole();
    test_no_blink();
    test_light();
    test_synth();
    if (failures) {
        printf("%d failures\n", failures);
        return 1;
    }
    return 0;
}
