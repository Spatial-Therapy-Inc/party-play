// Party Play 1.0 (yaykit/docs/product/party-play.md, party-play-ui.md): five offline party games for the round
// screen lying in the middle of the table. Start-up follows the extension template: NVS (never erased) ->
// yaykit_app -> board -> side key -> screen -> yk_app_ready() -> audio and motion -> console.
//
// The shared layer it stands on: the result and the record on leaving (P3 yk_app_on_leave, yk_app_set_result_summary,
// P19 yk_app_record); volume, brightness, screen-off time and language as Yay has them (yaykit_app, 0.1.13, which
// also puts the app to sleep when the battery runs out); the battery and the fast touch for
// the reaction game (P20); motion from yaykit_motion (P12); sounds through yaykit_speak's low-latency effect mixer
// (P13, pp_sound.c); `snap` from yaykit_ui. pp_ui.c draws the screen itself, Yay included (see the README).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "cjk_font.h"
#include "esp_console.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_psram.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "pp_gesture.h"
#include "pp_motion_map.h"
#include "pp_play.h"
#include "pp_sound.h"
#include "pp_ui.h"
#include "pp_util.h"
#include "yaykit_app.h"
#include "yaykit_key.h"
#include "yaykit_motion.h"
#include "yaykit_slots.h"
#include "yaykit_speak.h"
#include "yaykit_ui.h"
#include "yk_mem.h"
#include "yk_hint.h"

#define APP_SLUG "party-play"
// The app's own NVS namespace, yk_<slug without its hyphens> (devkit docs/app-guide.md 3). Up to 1.0.7 in the main
// repository it was yk_party: those settings (players, dice, forfeits, language, the ten's mode) start over once.
#define NVS_NS "yk_partyplay"

static const char *TAG = "party";

static pp_play_t s_play;
static pp_gesture_t s_g;
static bool s_ignore;            // this touch only woke the screen, or was already used
static float s_last_x, s_last_y;
static int64_t s_tick_us;        // when the last tick ran (play time s_play.now)
static volatile bool s_key_tap;
static int s_bright = -1, s_bright_on = BOARD_PREF_BRIGHTNESS_DEFAULT;
static bool s_muted, s_fast, s_holding = false;
static bool s_motion;  // yaykit_motion started (the sensor answered)
static int s_volume_applied = -1;
static float s_settings_at = -100, s_battery_at = -100;
static int64_t s_snap_at;  // when the snap sound was placed (for the log next to PARTY_GO_SHOWN)
static int64_t s_go_at;    // when the reaction game turned to GO (its lit frame drawn next)
static int s_false_seat = -1;    // console (party react false): a false start from this seat ...
static float s_false_before;     // ... this long before the lit frame
static uint32_t s_judged_serial;  // the last reaction judgement logged
static int s_go_ms = 25;   // GO to the middle of the screen lit, as the last lit frame measured it
// The lit frame band by band, as LVGL sends the screen (CONFIG_YK_BOARD_LCD_BUFFER_LINES rows at a time, from the
// top): where each band starts and when it was on the glass.
#define GO_BANDS 16
static int s_go_y1[GO_BANDS], s_go_n;
static int64_t s_go_glass[GO_BANDS];
static bool s_go_frame;
static uint32_t s_settings_sig;

// ───────────────────────── settings (NVS yk_party)
static uint32_t settings_sig(void) {
    return (uint32_t)(s_play.players | s_play.dice_n << 4 | s_play.volume << 8 | s_play.penalties << 13 | s_play.en << 14 |
                      s_play.ten.mode << 15 | s_play.quick_bomb << 20);
}

static void settings_load(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t v;
    if (nvs_get_u8(h, "players", &v) == ESP_OK) s_play.players = pp_clampi(v, 2, 8);
    if (nvs_get_u8(h, "dice", &v) == ESP_OK) s_play.dice_n = pp_clampi(v, 1, 3);
    if (nvs_get_u8(h, "pen", &v) == ESP_OK) s_play.penalties = v != 0;
    if (nvs_get_u8(h, "en", &v) == ESP_OK) s_play.en = v != 0;
    if (nvs_get_u8(h, "ten", &v) == ESP_OK) s_play.ten.mode = v ? 1 : 0;
    nvs_close(h);
}

static void settings_save(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "players", (uint8_t)s_play.players);
    nvs_set_u8(h, "dice", (uint8_t)s_play.dice_n);
    nvs_set_u8(h, "pen", s_play.penalties);
    nvs_set_u8(h, "en", s_play.en);
    nvs_set_u8(h, "ten", (uint8_t)s_play.ten.mode);
    nvs_commit(h);
    nvs_close(h);
    // the volume is the device's (P10): Yay and every app open with it (yaykit_app keeps it, 0.1.13)
    if (s_play.volume != yk_app_volume()) yk_app_set_volume(s_play.volume);
}

// ───────────────────────── the result for Yay and the session record
/** Written when the app leaves (P3): what Yay says and draws, and the session for the outbox (P19). Builds the text
 *  under the board lock (the game is the LVGL task's), writes NVS outside it. */
static void write_result(void) {
    char sentence[160], data[256], record[384], value[3][16], label[3][24];
    bool worth = false;
    int stats = 0;
    if (board_lock(250) != ESP_OK) return;
    pp_session_summary(&s_play.session, s_play.en, sentence, sizeof(sentence));
    stats = pp_session_stats(&s_play.session, s_play.en, value, label);
    if (pp_session_data_json(&s_play.session, data, sizeof(data)) < 0) strcpy(data, "{}");
    worth = pp_session_worth(&s_play.session) && pp_session_record_json(&s_play.session, record, sizeof(record)) > 0;
    board_unlock();
    yk_summary_t sum;
    yk_summary_init(&sum);
    for (int i = 0; i < stats; i++) yk_summary_stat(&sum, value[i], label[i]);
    cJSON *json = cJSON_Parse(data);
    yk_app_set_result_summary(sentence, &sum, json);
    cJSON_Delete(json);
    printf("PARTY_RESULT %s %s\n", sentence, data);
    // only a session worth keeping; the platform drops it unless the user agreed to records
    if (worth) {
        const esp_err_t err = yk_app_record("party.session", record);
        printf("PARTY_RECORD %s %s\n", err == ESP_OK ? "queued" : esp_err_to_name(err), record);
    }
}

/** yk_app_on_leave: Party Play is going home (double click, app exit). 400 ms: quiet the sound, leave the result. */
static void on_leave(void *ctx) {
    (void)ctx;
    pp_sound_mute(true);  // a 10 ms fade, not a cut
    write_result();
}

// ───────────────────────── each frame (LVGL timer: the board lock is held)
static float play_time_now(void) { return s_play.now + (esp_timer_get_time() - s_tick_us) / 1e6f; }

static void outputs(void) {
    pp_out_t o[32];
    int n;
    while ((n = pp_play_take(&s_play, o, 32)) > 0)
        for (int i = 0; i < n; i++) {
            switch (o[i].kind) {
            case PP_OUT_SOUND:
                if (o[i].id == PP_SND_SNAP_GO) {
                    // the reaction game's "snap" is heard when the middle of its lit frame lights (measured each time)
                    const int ahead = pp_sound_latency_ms() > s_go_ms ? pp_sound_latency_ms() : s_go_ms;
                    s_go_at = esp_timer_get_time();
                    s_snap_at = s_go_at + ahead * 1000;
                    pp_sound_play(PP_SND_SNAP_GO, o[i].gain, s_snap_at);
                } else {
                    pp_sound_play((pp_sound_t)o[i].id, o[i].gain, 0);
                }
                break;
            case PP_OUT_LINE:
                // Yay's lines are not voiced until the TTS terms are cleared (docs/product/party-play.md); logged for tests
                if (o[i].id == PP_LINE_PENALTY) {
                    const pp_penalty_t *pen = &PP_PENALTIES[pp_clampi((int)o[i].gain, 0, PP_PENALTY_COUNT - 1)];
                    printf("PARTY_SAY penalty %s\n", s_play.en ? pen->en_say : pen->zh_say);
                } else if (o[i].id == PP_LINE_SUM) {
                    printf("PARTY_SAY number %.2f\n", o[i].gain);
                } else {
                    printf("PARTY_SAY %s\n", pp_line_text((pp_line_t)o[i].id, s_play.en));
                }
                break;
            case PP_OUT_SCENE: printf("PARTY_SCENE %s\n", pp_scene_name((pp_scene_t)o[i].id)); break;
            default: break;
            }
        }
}

static void battery(void) {
    if (s_play.now - s_battery_at <= 30) return;
    s_battery_at = s_play.now;
    board_battery_t b;
    if (board_battery(&b) == ESP_OK && b.percent >= 0) {
        // on USB there is no "low": it is charging (round-screen-design.md 9)
        pp_play_battery(&s_play, b.usb ? 0 : b.percent < 5 ? 2 : b.percent < 15 ? 1 : 0, b.percent);
    } else {
        pp_play_battery(&s_play, 0, -1);  // no reading: never "low"
    }
}

// The light against the frames (`perf light`): brightness commands and frames as they happen, kept from the last read
// until full, so a test can check that nothing is shown lit before it is drawn (starting, a new scene, a turn, waking)
// without anyone watching the screen.
typedef struct {
    int64_t t;
    char kind;  // B brightness (v: percent), F a frame starts, R it is drawn (v: scene)
    int v;
} light_ev_t;
static light_ev_t s_lev[256];
static int s_lev_n;
static void light_ev(char kind, int v) {
    if (s_lev_n < (int)(sizeof(s_lev) / sizeof(s_lev[0]))) s_lev[s_lev_n++] = (light_ev_t){esp_timer_get_time(), kind, v};
}

static void brightness(int want) {
    // kept only once the panel took it: a failed command is sent again next tick, not left half way
    if (want != s_bright && board_brightness_set(want) == ESP_OK) {
        s_bright = want;
        light_ev('B', want);
    }
}

static void power(void) {
    battery();  // first: it may change the scene, whose light starts at 0
    // Yay's brightness and screen-off time as yaykit_app has them now: capped while the battery is low (no NVS read)
    s_bright_on = yk_app_brightness();
    s_play.idle_off = (float)yk_app_screen_off_s();
    const int dim = s_bright_on * 3 / 10 > 5 ? s_bright_on * 3 / 10 : 5;
    // a new scene and a turn come up from dark through the panel's brightness (pp_play_light): drawn once, unseen
    const int level = s_play.face_down || s_play.off ? 0 : s_play.dimmed ? dim : s_bright_on;
    brightness((int)lroundf(level * pp_play_light(&s_play)));
    if (s_play.face_down != s_muted) {
        s_muted = s_play.face_down;
        pp_sound_mute(s_muted);
    }
    // in a game the amplifier stays on (sounds come any moment); in the menu and pages, and with the screen off, it
    // may rest
    const bool hold = !s_muted && !s_play.off && s_play.scene >= PP_SC_SEATS && s_play.scene <= PP_SC_TEN;
    if (hold != s_holding) {
        s_holding = hold;
        pp_sound_hold_amp(hold);
    }
    if (s_play.volume != s_volume_applied) {
        yk_speak_volume(s_play.volume);
        s_volume_applied = s_play.volume;
    }
}

static void persist(void) {
    const uint32_t sig = settings_sig();
    if (sig != s_settings_sig && s_play.now - s_settings_at > 2) {
        s_settings_sig = sig;
        s_settings_at = s_play.now;
        settings_save();
    }
}

/** The reaction game times touches from the controller itself while a round is on (P20: every 5 ms, timestamped),
 *  not from LVGL's once-a-frame reading. */
static void fast_touch(void) {
    const pp_rx_state_t st = s_play.react.st;
    const bool want = s_play.scene == PP_SC_REACT && (st == PP_RX_WAIT || st == PP_RX_DECOY || st == PP_RX_GO);
    if (want && !s_fast) {
        s_fast = board_touch_fast_start(5) == ESP_OK;
        if (s_fast) board_touch_fast_clear();
    } else if (!want && s_fast) {
        board_touch_fast_stop();
        s_fast = false;
    }
    if (!s_fast) return;
    board_touch_edge_t e;
    while (board_touch_fast_take(&e)) {
        if (!e.pressed) continue;
        const float at = s_play.now + (e.t_us - s_tick_us) / 1e6f;  // play time of the contact
        pp_play_tap(&s_play, e.x, e.y, at);
    }
}

/** yaykit_motion's events since the last frame and its state now, in the game's terms. */
static void motion_take(pp_motion_in_t *mi) {
    yk_motion_event_t ev[16];
    int n = 0;
    yk_motion_state_t st;
    if (s_motion) {
        while (n < 16 && yk_motion_take(&ev[n])) n++;
        yk_motion_state(&st);
        yk_motion_holder(500, &st.holder_angle, &st.holder_confidence);  // the half second before (the bomb's loser)
    }
    pp_motion_map(ev, n, s_motion ? &st : NULL, s_motion, mi);
}

/** The hint arrow (P24): the first time the menu is up and lit since the start, where the app ring is (yk_hint APPS,
 *  until the ring was opened once); never over a game, the pages or a dark screen (a press takes it away in
 *  yaykit_app). Under the lock (the LVGL task). */
static bool s_apps_hint_due = true;
static void hint(void) {
    const bool menu = s_play.scene == PP_SC_MENU && !s_play.off && !s_play.face_down;
    if (!menu && yk_hint_is_shown()) yk_hint_hide();
    if (!s_apps_hint_due || !menu || pp_play_light(&s_play) < 0.99f) return;
    s_apps_hint_due = false;
    if (yk_hint_should(YK_HINT_APPS)) printf("PARTY_HINT apps shown=%d\n", yk_hint_show(lv_screen_active(), YK_HINT_APPS, NULL));
}

static void tick(lv_timer_t *t) {
    (void)t;
    fast_touch();  // before the clock moves: the touches' times are relative to the last tick
    const int64_t now = esp_timer_get_time();
    const float dt = s_tick_us ? (now - s_tick_us) / 1e6f : 0.033f;
    s_tick_us = now;
    if (s_key_tap) {
        s_key_tap = false;
        pp_play_key_short(&s_play);
    }
    pp_motion_in_t mi;
    motion_take(&mi);
    const pp_react_t *rx = &s_play.react;
    if (s_false_seat >= 0 && s_play.scene == PP_SC_REACT && rx->st == PP_RX_WAIT && rx->wait - rx->t <= s_false_before &&
        s_false_seat < s_play.seats.n) {
        const float a = s_play.seats.s[s_false_seat].a;
        pp_play_tap(&s_play, PP_C + 170 * cosf(a), PP_C + 170 * sinf(a), s_play.now);
        printf("PARTY_FALSE_START seat=%d before_go_ms=%d\n", s_false_seat, (int)((rx->wait - rx->t) * 1000));
        s_false_seat = -1;
    }
    pp_play_tick(&s_play, dt, &mi);
    if (s_play.judged.serial != s_judged_serial) {  // both counts, whichever one decided (react_by_part)
        s_judged_serial = s_play.judged.serial;
        printf("PARTY_REACT_JUDGED counted=%s whole_seat=%d whole_rt=%.3f part_seat=%d part_rt=%.3f\n",
               s_play.react_by_part ? "part" : "whole", s_play.judged.seat[0], s_play.judged.rt[0], s_play.judged.seat[1],
               s_play.judged.rt[1]);
    }
    outputs();
    power();
    persist();
    hint();
    pp_ui_frame();
}

static bool go_pending(void) { return s_play.scene == PP_SC_REACT && s_play.react.st == PP_RX_GO && s_play.go_shown == 0; }

static void on_refr_start(lv_event_t *e) {
    (void)e;
    // a scene changed between ticks (a touch) is due dark: dark before any of it reaches the glass
    if (pp_play_light(&s_play) == 0 && s_bright != 0) brightness(0);
    light_ev('F', s_play.scene);
    s_go_frame = go_pending();
    s_go_n = 0;
}

static void on_flush_start(lv_event_t *e) {
    if (!s_go_frame || s_go_n >= GO_BANDS) return;
    const lv_area_t *a = lv_event_get_param(e);
    // on the glass once sent: 2 bytes a pixel over the QSPI bus at 80 MHz, 4 bits a clock (40 bytes a microsecond)
    s_go_y1[s_go_n] = a->y1;
    s_go_glass[s_go_n++] = esp_timer_get_time() + (int64_t)lv_area_get_size(a) * 2 / 40;
}

static void on_refr_ready(lv_event_t *e) {
    (void)e;
    light_ev('R', s_play.scene);
    // the reaction game's lit frame is on the glass, band by band: the whole of it (everyone's start by default) and
    // each seat's part (react_by_part)
    if (s_go_frame && go_pending() && s_go_n > 0) {
        float at[GO_BANDS];
        int mid = 0;
        for (int k = 0; k < s_go_n; k++) {
            at[k] = s_play.now + (s_go_glass[k] - s_tick_us) / 1e6f;
            if (s_go_y1[k] <= PP_C) mid = k;
        }
        pp_play_go_bands(&s_play, s_go_y1, at, s_go_n);
        const int frame_ms = (int)((s_go_glass[mid] - s_go_at) / 1000);
        if (frame_ms > 0 && frame_ms < 300) s_go_ms = frame_ms;
        // the snap was placed to be heard at s_snap_at: against the middle of the screen, the sync error
        printf("PARTY_GO_SHOWN at=%.3f bands=%d sweep_ms=%d frame_ms=%d sound_minus_frame_ms=%d light=%.2f bright=%d\n",
               s_play.go_shown, s_go_n, (int)((s_go_glass[s_go_n - 1] - s_go_glass[0]) / 1000), frame_ms,
               (int)((s_snap_at - s_go_glass[mid]) / 1000), pp_play_light(&s_play), s_bright);
    }
    s_go_frame = false;
}

// ───────────────────────── touch (LVGL task) and the side key (key task)
static void on_touch(int phase, float x, float y) {
    const float t = esp_timer_get_time() / 1e6f;
    yk_app_input();
    if (phase == 0) {
        if (!s_holding) yk_speak_prepare();  // outside a game the amplifier rests; wake it for the click
        s_ignore = !pp_play_touch_down(&s_play, x, y);
        s_last_x = x, s_last_y = y;
        // during a reaction round the fast touch reading has this press already (fast_touch); without it, the moment
        // of contact still counts, not the release
        const pp_rx_state_t st = s_play.react.st;
        if (!s_ignore && s_play.scene == PP_SC_REACT && (st == PP_RX_WAIT || st == PP_RX_DECOY || st == PP_RX_GO)) {
            if (!s_fast) pp_play_tap(&s_play, x, y, play_time_now());
            s_ignore = true;
        }
        if (!s_ignore) pp_gesture_down(&s_g, x, y, t, pp_play_rim_min(&s_play));
        return;
    }
    if (s_ignore) {
        // two people at once: the controller's single point jumps between them
        if (phase == 1 && s_play.scene == PP_SC_REACT && s_play.react.st == PP_RX_GO && hypotf(x - s_last_x, y - s_last_y) > 120)
            pp_play_two_at_once(&s_play);
        s_last_x = x, s_last_y = y;
        if (phase == 2) s_ignore = false;
        return;
    }
    s_last_x = x, s_last_y = y;
    if (phase == 1) {
        const pp_gesture_ev_t ev = pp_gesture_move(&s_g, x, y, t);
        if (ev == PP_G_RIM_START) pp_play_rim_start(&s_play, s_g.x0, s_g.y0);
        if (ev == PP_G_RIM_START || ev == PP_G_RIM_MOVE) pp_play_rim_move(&s_play, s_g.da);
        return;
    }
    const pp_gesture_ev_t ev = pp_gesture_up(&s_g, x, y, t, s_play.rot);
    switch (ev) {
    case PP_G_TAP: pp_play_tap(&s_play, s_g.x0, s_g.y0, play_time_now()); break;
    case PP_G_RIM_END: pp_play_rim_end(&s_play, s_g.w); break;
    case PP_G_SWIPE_LEFT:
    case PP_G_SWIPE_RIGHT:
    case PP_G_SWIPE_DOWN:
    case PP_G_SWIPE_UP: pp_play_swipe(&s_play, ev); break;
    default: break;
    }
}

static bool on_key(const yk_key_event_t *ev, void *ctx) {
    (void)ctx;
    if (ev->type == YK_KEY_TAP) s_key_tap = true;
    // DOUBLE belongs to yaykit_app: the app ring. HOLD is not used by Party Play (round-screen-design.md 6):
    // yaykit_app asks 「回到 Yay 说？」 — but not on a dark screen or face down, where nobody would see the card.
    return ev->type == YK_KEY_TAP || (ev->type == YK_KEY_HOLD && (s_play.off || s_play.face_down));
}

// ───────────────────────── console
/** `yk info` and `yk cmd <name> [json]` over USB, in yaykit_link's format (tools/yay_dev.py, the installer, the
 *  tests read them). Party Play does not start yaykit_link (it has no Bluetooth), so it answers these itself: info
 *  without the Bluetooth parts, the commands through yk_app_command (app.exit, app.status, app.recovery). */
static const char *reset_reason(void) {
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "poweron";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT: return "wdt";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_USB: return "usb";
    default: return "unknown";
    }
}

static void yk_info(void) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "proto", 1);
    cJSON *device = cJSON_AddObjectToObject(root, "device");
    // the device id as yaykit_link makes it (yk_ + the first 6 bytes of SHA-256(MAC | "yaykit")): Party Play never
    // starts the link, and yk_sys is the main firmware's to read (it holds the account's token)
    char id[24] = "";
    uint8_t seed[12], digest[32];
    if (esp_efuse_mac_get_default(seed) == ESP_OK) {
        memcpy(seed + 6, "yaykit", 6);
        mbedtls_sha256(seed, sizeof(seed), digest, 0);
        strcpy(id, "yk_");
        for (int i = 0; i < 6; i++) sprintf(id + 3 + i * 2, "%02x", digest[i]);
    }
    cJSON_AddStringToObject(device, "id", id);
    cJSON_AddStringToObject(device, "board", "tikpal-r1");
    cJSON_AddStringToObject(device, "chip", "ESP32-S3");
    uint32_t flash = 0;
    esp_flash_get_size(NULL, &flash);
    cJSON_AddNumberToObject(device, "flash", flash);
    cJSON_AddNumberToObject(device, "psram", esp_psram_is_initialized() ? esp_psram_get_size() : 0);
    cJSON *app = cJSON_AddObjectToObject(root, "app");
    cJSON_AddStringToObject(app, "slug", APP_SLUG);
    cJSON_AddStringToObject(app, "version", PARTY_VERSION);
    cJSON_AddStringToObject(app, "layout", PARTY_LAYOUT);
    yk_slots_to_json(cJSON_AddArrayToObject(root, "slots"));
    cJSON *heap = cJSON_AddObjectToObject(root, "heap");
    cJSON_AddNumberToObject(heap, "internalFree", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    // the same figures as yaykit_link's yk info: the 40,000 bytes line is read on the lowest since yk_app_ready()
    cJSON_AddNumberToObject(heap, "internalMinSinceReady", yk_mem_min_since_ready());
    cJSON_AddNumberToObject(heap, "internalMinEver", yk_mem_min_ever());
    cJSON_AddNumberToObject(heap, "internalLargest", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(heap, "psramFree", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddStringToObject(cJSON_AddObjectToObject(root, "ble"), "state", "off");
    cJSON_AddStringToObject(cJSON_AddObjectToObject(root, "wifi"), "state", "off");
    cJSON_AddNumberToObject(root, "uptime", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddStringToObject(root, "resetReason", reset_reason());
    char *text = cJSON_PrintUnformatted(root);
    printf("YK_INFO %s\n", text ? text : "{}");
    cJSON_free(text);
    cJSON_Delete(root);
}

static int yk_cmd(int argc, char **argv) {
    yk_app_input();
    if (argc >= 2 && !strcmp(argv[1], "info")) {
        yk_info();
        return 0;
    }
    if (argc < 3 || strcmp(argv[1], "cmd")) {
        puts("usage: yk info | yk cmd <name> [json]");
        return 1;
    }
    char json[256] = "";
    for (int i = 3; i < argc; i++) {  // the console split the JSON on spaces: join it back
        if (i > 3) strlcat(json, " ", sizeof(json));
        strlcat(json, argv[i], sizeof(json));
    }
    cJSON *args = *json ? cJSON_Parse(json) : NULL;
    if (*json && !args) {
        puts("YK_ERR BAD_JSON");
        return 1;
    }
    cJSON *reply = cJSON_CreateObject();
    const char *code = NULL;
    esp_err_t err = yk_app_command(argv[2], args, reply, &code);  // app.exit, app.status, app.recovery
    if (err == ESP_ERR_NOT_SUPPORTED && !strcmp(argv[2], "party.status")) {
        board_lock(-1);
        cJSON_AddStringToObject(reply, "scene", pp_scene_name(s_play.scene));
        cJSON_AddNumberToObject(reply, "seats", s_play.seats.n);
        cJSON_AddNumberToObject(reply, "rounds", pp_session_total_rounds(&s_play.session));
        board_unlock();
        err = ESP_OK;
    } else if (err == ESP_ERR_NOT_SUPPORTED) {
        code = "UNKNOWN_COMMAND";
    }
    if (err == ESP_OK) {
        char *text = cJSON_PrintUnformatted(reply);
        printf("YK_OK %s\n", text ? text : "{}");
        cJSON_free(text);
    } else {
        printf("YK_ERR %s\n", code ? code : esp_err_to_name(err));
    }
    cJSON_Delete(reply);
    cJSON_Delete(args);
    return err == ESP_OK ? 0 : 1;
}

static int heap_cmd(int argc, char **argv) {
    (void)argc, (void)argv;
    printf("HEAP internal=%u internal_min=%u psram=%u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return 0;
}

#if CONFIG_LV_USE_PROFILER_BUILTIN
#include "debugging/profiler/lv_profiler_builtin_private.h"
// `perf trace`: LVGL's built-in profiler, in the profiling build only (sdkconfig.profile, see the README)
static uint64_t trace_tick(void) { return (uint64_t)esp_timer_get_time(); }
static void trace_out(const char *line) { fputs(line, stdout); }

static int perf_trace(int argc, char **argv) {
    const bool live = argc > 2 && !strcmp(argv[2], "live");
    const int n = argc > 3 ? atoi(argv[3]) : live ? 1000 : 3;
    board_lock(-1);
    // a fresh buffer each time (a flush does not empty it): microseconds instead of lv_init()'s milliseconds
    lv_profiler_builtin_config_t c;
    lv_profiler_builtin_config_init(&c);
    c.buf_size = live ? 3 << 20 : 1 << 20;  // PSRAM; a full buffer prints itself mid-trace, stalling the screen
    c.tick_per_sec = 1000000;
    c.tick_get_cb = trace_tick;
    c.flush_cb = trace_out;
    lv_profiler_builtin_init(&c);
    lv_profiler_builtin_set_enable(true);
    if (live) {  // what the screen does on its own for n ms (a spinning menu, a lit frame)
        if (argc > 4 && !strcmp(argv[4], "flick")) {  // the menu flicked as the trace starts, so it sees all of it
            pp_play_rim_start(&s_play, 233, 20);
            pp_play_rim_move(&s_play, 40 * PP_PI / 180);
            pp_play_rim_end(&s_play, 12);
        }
        board_unlock();
        vTaskDelay(pdMS_TO_TICKS(n));
        board_lock(-1);
    } else {  // n whole frames of the scene as it is
        for (int i = 0; i < n; i++) {
            pp_ui_invalidate_all();
            lv_refr_now(NULL);
        }
    }
    lv_profiler_builtin_set_enable(false);
    printf("PERF_TRACE_BEGIN scene=%s %s %d\n", pp_scene_name(s_play.scene), live ? "live" : "full", n);
    lv_profiler_builtin_flush();
    printf("PERF_TRACE_END\n");
    board_unlock();
    return 0;
}
#endif

static int perf_cmd(int argc, char **argv) {
#if CONFIG_LV_USE_PROFILER_BUILTIN
    if (argc > 1 && !strcmp(argv[1], "trace")) return perf_trace(argc, argv);
#endif
    if (argc > 1 && !strcmp(argv[1], "light")) {  // the brightness and frames since the last read, then cleared
        board_lock(-1);
        printf("PERF_LIGHT n=%d full=%d", s_lev_n, s_lev_n == (int)(sizeof(s_lev) / sizeof(s_lev[0])));
        for (int i = 0; i < s_lev_n; i++) printf(" %lld%c%d", (long long)(s_lev[i].t / 1000), s_lev[i].kind, s_lev[i].v);
        printf("\n");
        s_lev_n = 0;
        board_unlock();
        return 0;
    }
    pp_ui_stats_t u;
    yk_speak_stats_t s;
    pp_ui_stats(&u, argc > 1 && !strcmp(argv[1], "reset"));
    yk_speak_stats(&s);
    printf("PERF frames=%u render_avg_ms=%u render_max_ms=%u full=%u px_avg=%u bg_max_ms=%u rate=%u block_ms=%u latency_ms=%d "
           "sfx_started=%u sfx_dropped=%u mix_us=%u mix_max_us=%u late_blocks=%u limited=%u scene=%s\n",
           (unsigned)u.frames, (unsigned)u.render_ms_avg, (unsigned)u.render_ms_max, (unsigned)u.full, (unsigned)u.px_avg,
           (unsigned)u.bg_ms_max, (unsigned)s.rate, (unsigned)s.block_ms, pp_sound_latency_ms(), (unsigned)s.sfx_started, (unsigned)s.sfx_dropped,
           (unsigned)s.mix_us, (unsigned)s.mix_max_us, (unsigned)s.late_blocks, (unsigned)s.limited, pp_scene_name(s_play.scene));
    return 0;
}

static int scene_by_name(const char *n) {
    for (int i = 0; i < PP_SC_COUNT; i++)
        if (!strcmp(n, pp_scene_name((pp_scene_t)i))) return i;
    return -1;
}

static int party_cmd(int argc, char **argv) {
    yk_app_input();
    if (argc < 2 || !strcmp(argv[1], "status")) {
        board_lock(-1);
        char angles[8 * 5 + 1] = "-";
        for (int i = 0, n = 0; i < s_play.seats.n && n < (int)sizeof(angles) - 5; i++)
            n += snprintf(angles + n, sizeof(angles) - n, "%s%d", i ? "," : "",
                          (int)lroundf(s_play.seats.s[i].a * 180 / PP_PI + 360) % 360);
        printf("PARTY_STATUS scene=%s seats=%d players=%d dice=%d volume=%d penalties=%d en=%d battery=%d idle=%.0f "
               "rounds=%d motion=%d fast_touch=%d quick=%d seat_deg=%s version=%s layout=%s\n",
               pp_scene_name(s_play.scene), s_play.seats.n, s_play.players, s_play.dice_n, s_play.volume, s_play.penalties,
               s_play.en, s_play.battery, s_play.idle, pp_session_total_rounds(&s_play.session), s_motion, s_fast,
               s_play.quick_bomb, angles, PARTY_VERSION, PARTY_LAYOUT);
        board_unlock();
        return 0;
    }
    const char *c = argv[1];
    const char *a = argc > 2 ? argv[2] : "";
    board_lock(-1);
    int rc = 0;
    if (!strcmp(c, "go") && scene_by_name(a) >= 0) {
        pp_play_go(&s_play, (pp_scene_t)scene_by_name(a));
    } else if (!strcmp(c, "tap") && argc > 3) {
        if (pp_play_touch_down(&s_play, atof(argv[2]), atof(argv[3]))) pp_play_tap(&s_play, atof(argv[2]), atof(argv[3]), play_time_now());
    } else if (!strcmp(c, "swipe")) {
        const int d = a[0] == 'l' ? PP_G_SWIPE_LEFT : a[0] == 'r' ? PP_G_SWIPE_RIGHT : a[0] == 'u' ? PP_G_SWIPE_UP : PP_G_SWIPE_DOWN;
        pp_play_swipe(&s_play, d);
    } else if (!strcmp(c, "rim")) {  // turn along the edge by this many degrees (and flick when a third value is given)
        pp_play_rim_start(&s_play, 233, 20);
        pp_play_rim_move(&s_play, (float)atof(a) * PP_PI / 180);
        pp_play_rim_end(&s_play, argc > 3 ? (float)atof(argv[3]) : 0);
    } else if (!strcmp(c, "key")) {
        pp_play_key_short(&s_play);
    } else if (!strcmp(c, "players") && *a) {
        s_play.players = pp_clampi(atoi(a), 2, 8);
        pp_seats_even(&s_play.seats, s_play.players);
    } else if (!strcmp(c, "dice") && *a) {
        s_play.dice_n = pp_clampi(atoi(a), 1, 3);
    } else if (!strcmp(c, "lang")) {
        s_play.en = !strcmp(a, "en");
        yk_ui_set_english(s_play.en);
        pp_ui_invalidate_all();
    } else if (!strcmp(c, "quick")) {
        s_play.quick_bomb = !strcmp(a, "on");
    } else if (!strcmp(c, "battery") && *a) {
        pp_play_battery(&s_play, pp_clampi(atoi(a), 0, 2), argc > 3 ? atoi(argv[3]) : -1);
        s_battery_at = s_play.now + 1e6f;  // keep the test value
    } else if (!strcmp(c, "face")) {
        pp_play_face_down(&s_play, !strcmp(a, "down") || !strcmp(a, "on"));
    } else if (!strcmp(c, "lift")) {  // as if picked up and shaken (dice) without the sensor
        pp_motion_in_t mi = {.events = 1 | 4, .held = false, .moving = true, .holder = 1.5707963f, .holder_conf = 1};
        pp_play_tick(&s_play, 0.001f, &mi);
    } else if (!strcmp(c, "react") && !strcmp(a, "false") && argc > 4) {  // a false start this many ms before GO
        s_false_seat = atoi(argv[3]);
        s_false_before = (float)atof(argv[4]) / 1000;
    } else if (!strcmp(c, "react") && !strcmp(a, "bypart") && argc > 3) {  // count by each seat's part (not kept)
        s_play.react_by_part = !strcmp(argv[3], "on");
    } else if (!strcmp(c, "result")) {
        board_unlock();
        write_result();
        board_lock(-1);
    } else {
        rc = 1;
    }
    board_unlock();
    if (rc) printf("usage: party status|go <scene>|tap <x> <y>|swipe l|r|u|d|rim <deg> [w]|key|players <n>|dice <n>|"
                   "lang zh|en|quick on|off|battery <0-2> [pct]|face down|up|lift|react false <seat> <ms>|"
                   "react bypart on|off|result\n");
    return rc;
}

static int sfx_cmd(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: sfx <0-%d> [gain]\n", PP_SND_COUNT - 1);
        return 1;
    }
    pp_sound_play((pp_sound_t)pp_clampi(atoi(argv[1]), 0, PP_SND_COUNT - 1), argc > 2 ? (float)atof(argv[2]) : 1, 0);
    return 0;
}

static void start_console(void) {
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    cfg.prompt = "party>";
    cfg.task_stack_size = 8192;
    esp_console_dev_usb_serial_jtag_config_t usb = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&usb, &cfg, &repl));
    esp_console_register_help_command();
    ESP_ERROR_CHECK(yk_app_register_console());
    yk_ui_register_snapshot();
    yk_motion_register_console();  // motion, motion log, motion events, motion cal (development builds)
    yk_speak_register_console();
    const esp_console_cmd_t cmds[] = {
        {.command = "yk", .help = "yk info | yk cmd <name> [json] (yaykit_link's format, over USB only)", .func = yk_cmd},
        {.command = "heap", .help = "available memory", .func = heap_cmd},
        {.command = "perf", .help = "frame and sound timing [reset]; profiling build: perf trace [full <frames> | live <ms>]", .func = perf_cmd},
        {.command = "party", .help = "game state and test input (party with no arguments shows usage)", .func = party_cmd},
        {.command = "sfx", .help = "play a sound by number", .func = sfx_cmd},
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}

void app_main(void) {
    // 1. NVS, never erased (shared with every app; holds the Wi-Fi credentials)
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) ESP_LOGE(TAG, "NVS unavailable (%s); running without saved settings", esp_err_to_name(err));
    // 2. yaykit_app first: the crash loop guard counts anything below; it also takes the double click (the app ring)
    ESP_ERROR_CHECK(yk_app_start(&(yk_app_config_t){.slug = APP_SLUG, .version = PARTY_VERSION}));
    // 3. board and screen, then the side key
    if (!board_display_start_opts(&(board_display_opts_t){.start_dark = true})) {  // dark until the first frame (P10)
        ESP_LOGE(TAG, "display failed");
        return;
    }
    // delay_tap: the first press of a double click is not a short press (back to the menu, a fuse put out): the ring
    // opens over the game, which goes on.
    ESP_ERROR_CHECK(yk_key_init(&(yk_key_config_t){.delay_tap = true}));
    ESP_ERROR_CHECK(yk_app_on_key(on_key, NULL));
    // 4. the game, with saved settings and Yay's launch parameters
    pp_play_init(&s_play, esp_random());
    settings_load();
    // Yay's settings (yaykit_app, 0.1.13): the volume, the screen-off time, the brightness and the language (the
    // owner's: the game's own "en" is only the console's `party lang` now)
    s_play.volume = yk_app_volume();
    s_play.idle_off = (float)yk_app_screen_off_s();
    s_bright_on = yk_app_brightness();
    s_play.en = yk_app_english();
    s_settings_sig = settings_sig();
    yk_launch_t launch;
    if (yk_app_launch(&launch) == ESP_OK && launch.present) {
        pp_launch_params_t lp;
        pp_launch_parse(launch.params, &lp);
        printf("PARTY_LAUNCH intent=%s params=%s game=%d players=%d dice=%d\n", launch.intent, launch.params, lp.game, lp.players, lp.dice);
        pp_play_launch(&s_play, &lp);
    }
    // 5. the screen; then confirm the image
    board_lock(-1);
    cjk_font_set_language(s_play.en ? "en" : "zh");
    yk_ui_set_english(s_play.en);  // yaykit_ui's lines (the hint, the ring) in the game's language
    ESP_ERROR_CHECK(cjk_font_init());
    pp_ui_init(&s_play, on_touch);
    lv_display_add_event_cb(lv_display_get_default(), on_refr_start, LV_EVENT_REFR_START, NULL);
    lv_display_add_event_cb(lv_display_get_default(), on_flush_start, LV_EVENT_FLUSH_START, NULL);
    lv_display_add_event_cb(lv_display_get_default(), on_refr_ready, LV_EVENT_REFR_READY, NULL);
    s_tick_us = esp_timer_get_time();
    lv_timer_create(tick, 33, NULL);
    lv_refr_now(NULL);  // the first frame is drawn before the panel lights up
    // The panel is still dark (P10) and stays so: power() brings it up with the menu's light (no flash at full first).
    // The brightness goes over the same panel I/O as LVGL's pixels, which is not safe from two tasks at once (esp_lcd's
    // tx_param recycles the transactions in flight: a dark screen that never starts, about 1 start in 30), so it is
    // only ever set from the LVGL task (power(), on_refr_start()), under the lock.
    s_bright = 0;
    light_ev('B', 0);  // where the light starts
    board_unlock();
    yk_app_on_leave(on_leave, NULL);
    yk_app_ready();
    // 6. sound (yaykit_speak's effect mixer, 24 kHz, 5 ms blocks) and motion (yaykit_motion, 224 Hz)
    if (pp_sound_start() == ESP_OK) {
        yk_speak_volume(s_play.volume);
        s_volume_applied = s_play.volume;
    } else {
        ESP_LOGE(TAG, "audio failed: no sound");
    }
    s_motion = yk_motion_start(NULL) == ESP_OK;
    if (!s_motion) ESP_LOGW(TAG, "no motion sensor: dice by tap only, the bomb never nags");
    // 7. console last: tools talk to it
    start_console();
}
