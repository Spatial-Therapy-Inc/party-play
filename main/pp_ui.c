#include "pp_ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cjk_font.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "pp_util.h"
#include "yk_yay.h"

// Colours: the round screen's palette (round-screen-design.md 3). yaykit_ui v2's theme (P11) takes them over.
#define PAPER 0x171C19
#define INK 0xFAF7EF
#define INK2 0xA5AA9E
#define INK3 0x737A70
#define PANEL 0x30372F
#define CORAL 0xDF795D
#define CORAL_TEXT 0xEE9A80
#define SAGE 0x9FB38C
#define SAGE_TEXT 0xB9CAA8
#define SAGE_DEEP 0x2B3527
#define AMBER 0xE3A857
#define CREAM 0xF3E6C8
#define DIE_FACE 0xF6EFE0
#define PIP 0x2A2F2B
#define PIP_RED 0xD2513C
#define BOMB 0x262B27
#define BOMB_RIM 0x5B645C

#define FACE_PX 96  // die faces are drawn once at this size, then rotated and scaled

static const char *TAG = "pp_ui";

static pp_play_t *P;
static pp_ui_touch_fn s_touch;
static lv_obj_t *s_stage;
static yk_yay_t *s_yay;
static lv_draw_buf_t *s_face[7];
static uint32_t s_yay_serial = UINT32_MAX;
static pp_ui_stats_t s_st;
// A scene's still part (the menu's ring, the seats' dots and hints, the reaction game's slices and seats) is drawn
// once into a full-screen buffer in PSRAM and pasted each frame; only what moves is drawn every time.
static lv_draw_buf_t *s_bg;
static lv_obj_t *s_bg_canvas;
static uint32_t s_bg_key;   // what the buffer holds now; 0: no cache in use
static uint32_t s_bg_want;  // what the scene's still part is now (0: this scene draws it all every frame)
#define BG_WORTH_PX 60000   // a redraw this big would draw the still part again: buffer it first
enum { PART_BG = 1, PART_FG = 2, PART_ALL = 3 };
static int s_part = PART_ALL;
#define BG (s_part & PART_BG)
#define FG (s_part & PART_FG)
// The menu's five icons, drawn once (for the way the middle faces) into small images and scaled as they turn round
#define ICON_PX 64  // holds an icon of size 23 (the nearest one's), the bomb's fuse spark too
#define ICON_SIZES 4  // 19..23: the nearest size pasted as it is (scaling would cost a transform per icon per frame)
static lv_draw_buf_t *s_icon[5][ICON_SIZES];
// particles as coverage masks (A8) drawn in their colour without a transform: a wide line or a disc costs LVGL its
// masks for every piece in every band
#define SPR_PX 24
#define CONF_ANGLES 16  // a confetti piece looks the same turned half way round
#define CONF_SIZES 3
#define DOTS 9          // radius 2..6 by halves
static lv_draw_buf_t *s_conf[CONF_SIZES][CONF_ANGLES], *s_dot[DOTS];
static float s_icon_rot = 1e9f;
// the menu's middle (the chosen game's big icon, its name and the hint) for each game, drawn ahead while the menu
// stands still (ARGB8888, PSRAM): a flick then pastes it instead of drawing an icon and turned text
#define CENTER_PX 256
static lv_draw_buf_t *s_center[5];
static uint32_t s_center_key[5];  // the facing and language each was drawn for (0: none)
// ten seconds' 88 px number: "0123456789." turned the way the middle faces, each drawn once as a coverage mask
// centred on its turning point (the baseline's middle), then pasted in its colour instead of a turned glyph a frame
#define GLYPH_PX 160
#define GLYPH_SIZE 88
static lv_draw_buf_t *s_glyph[11];  // cropped to the ink: an empty pixel of a mask costs as much to blend as a full one
static lv_point_t s_glyph_off[11];   // the crop's top left from the turning point
static float s_glyph_rot = 1e9f;
static int64_t s_refr_start;
static uint32_t s_bg_ms, s_full_px;  // this frame's cache drawing (background, icons, middle, digits); its pixels
static uint64_t s_render_total, s_px_total;

// Yay hosting: the rest of the screen under 78% paper (round-screen-design.md 7). Drawn in colours already mixed with
// paper, images recoloured towards it, rather than under a full-screen veil every redrawn pixel would blend through:
// paper * v + (under * (1 - a) + c * a) * (1 - v) is the same picture.
#define VEIL_OPA 200
static bool s_veil;
static lv_color_t col(uint32_t hex) {
    return s_veil ? lv_color_mix(lv_color_hex(PAPER), lv_color_hex(hex), VEIL_OPA) : lv_color_hex(hex);
}
static void veil_image(lv_draw_image_dsc_t *d) {
    if (s_veil) d->recolor = lv_color_hex(PAPER), d->recolor_opa = VEIL_OPA;
}
static int ri(float v) { return (int)lroundf(v); }

// ───────────────────────── drawing helpers
/** True when the box (screen pixels) reaches what the layer draws now, one band of one redrawn area. Every redrawn
 *  area runs the whole scene, and LVGL makes and runs a draw task even for what lies outside it (a letter still looks
 *  up its glyph), so the helpers skip those first. */
static bool on(const lv_layer_t *L, float x0, float y0, float x1, float y1) {
    const lv_area_t *c = &L->_clip_area;
    return x1 >= c->x1 - 1 && x0 <= c->x2 + 1 && y1 >= c->y1 - 1 && y0 <= c->y2 + 1;
}

/** The box around a ring's sector (radii r0..r1, angles lo..hi): its four corners and the outer edge wherever it
 *  crosses an axis. b: x0, y0, x1, y1. */
static void sector_box(float cx, float cy, float r0, float r1, float lo, float hi, float b[4]) {
    b[0] = b[1] = 1e9f, b[2] = b[3] = -1e9f;
    float a[16];
    int n = 0;
    a[n++] = lo, a[n++] = hi;
    for (int k = (int)ceilf(lo / (PP_PI / 2)); k * (PP_PI / 2) <= hi && n < 16; k++) a[n++] = k * (PP_PI / 2);
    for (int i = 0; i < n; i++)
        for (int j = 0; j < 2; j++) {
            if (i >= 2 && j == 0) continue;  // at an axis only the outer edge reaches out
            const float r = j ? r1 : r0, x = cx + r * cosf(a[i]), y = cy + r * sinf(a[i]);
            b[0] = fminf(b[0], x), b[1] = fminf(b[1], y), b[2] = fmaxf(b[2], x), b[3] = fmaxf(b[3], y);
        }
}

static void disc(lv_layer_t *L, float x, float y, float r, uint32_t c, lv_opa_t opa) {
    if (r < 0.5f || !on(L, x - r, y - r, x + r, y + r)) return;
    LV_PROFILER_BEGIN_TAG("pp_disc");
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = col(c);
    d.bg_opa = opa;
    d.radius = LV_RADIUS_CIRCLE;
    const lv_area_t a = {ri(x - r), ri(y - r), ri(x + r) - 1, ri(y + r) - 1};
    lv_draw_rect(L, &d, &a);
    LV_PROFILER_END_TAG("pp_disc");
}

/** The whole screen in one colour: a plain rectangle, not a radius-233 circle (whose corner masks cost every row of
 *  the screen; the glass is round, so the square's corners are never seen). */
static void fill_screen(lv_layer_t *L, uint32_t c, lv_opa_t opa) {
    if (opa < 2) return;
    LV_PROFILER_BEGIN_TAG("pp_fill");
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = col(c);
    d.bg_opa = opa;
    const lv_area_t a = {0, 0, PP_W - 1, PP_W - 1};
    lv_draw_rect(L, &d, &a);
    LV_PROFILER_END_TAG("pp_fill");
}

static void arc(lv_layer_t *L, float cx, float cy, float r_out, float width, float a0, float a1, uint32_t c, lv_opa_t opa,
                bool rounded) {
    if (opa < 2 || width < 1 || a1 - a0 < 0.009f) return;  // an empty arc (LVGL would read equal ends as nothing or all)
    if (a1 - a0 >= PP_TAU - 0.01f) {
        if (!on(L, cx - r_out, cy - r_out, cx + r_out, cy + r_out)) return;
    } else {
        float b[4];
        sector_box(cx, cy, r_out - width, r_out, a0, a1, b);
        const float m = rounded ? width / 2 + 1 : 1;
        if (!on(L, b[0] - m, b[1] - m, b[2] + m, b[3] + m)) return;
    }
    LV_PROFILER_BEGIN_TAG("pp_arc");
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.color = col(c);
    d.opa = opa;
    d.width = ri(width);
    d.radius = (uint16_t)ri(r_out);
    d.center = (lv_point_t){ri(cx), ri(cy)};
    d.rounded = rounded;
    float s = a0 * 180 / PP_PI, span = (a1 - a0) * 180 / PP_PI;
    if (span >= 359.5f) {
        d.start_angle = 0;
        d.end_angle = 360;
    } else {
        s = fmodf(s, 360);
        if (s < 0) s += 360;
        float e = s + span;
        if (e >= 360) e -= 360;
        d.start_angle = ri(s);
        d.end_angle = ri(e);
    }
    lv_draw_arc(L, &d);
    LV_PROFILER_END_TAG("pp_arc");
}

static void band(lv_layer_t *L, float r0, float r1, float a0, float a1, uint32_t c, lv_opa_t opa) {
    arc(L, PP_C, PP_C, r1, r1 - r0, a0, a1, c, opa, false);
}

static void tri(lv_layer_t *L, float x0, float y0, float x1, float y1, float x2, float y2, uint32_t c, lv_opa_t opa) {
    if (!on(L, fminf(x0, fminf(x1, x2)), fminf(y0, fminf(y1, y2)), fmaxf(x0, fmaxf(x1, x2)), fmaxf(y0, fmaxf(y1, y2)))) return;
    LV_PROFILER_BEGIN_TAG("pp_tri");
    lv_draw_triangle_dsc_t d;
    lv_draw_triangle_dsc_init(&d);
    d.p[0].x = ri(x0), d.p[0].y = ri(y0);
    d.p[1].x = ri(x1), d.p[1].y = ri(y1);
    d.p[2].x = ri(x2), d.p[2].y = ri(y2);
    d.color = col(c);
    d.opa = opa;
    lv_draw_triangle(L, &d);
    LV_PROFILER_END_TAG("pp_tri");
}

static void line(lv_layer_t *L, float x0, float y0, float x1, float y1, float w, uint32_t c, lv_opa_t opa) {
    if (!on(L, fminf(x0, x1) - w, fminf(y0, y1) - w, fmaxf(x0, x1) + w, fmaxf(y0, y1) + w)) return;
    LV_PROFILER_BEGIN_TAG("pp_line");
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.p1.x = ri(x0), d.p1.y = ri(y0);
    d.p2.x = ri(x1), d.p2.y = ri(y1);
    d.width = ri(w);
    d.color = col(c);
    d.opa = opa;
    d.round_start = d.round_end = 1;
    lv_draw_line(L, &d);
    LV_PROFILER_END_TAG("pp_line");
}

/** A point (x, y) in the frame of whoever the middle faces, turned onto the screen around the centre. */
static void to_screen(float rot, float x, float y, float *sx, float *sy) {
    const float c = cosf(rot), s = sinf(rot);
    *sx = PP_C + x * c - y * s;
    *sy = PP_C + x * s + y * c;
}

/** Decodes one UTF-8 character; returns its length (1 on a broken byte). */
static int utf8(const char *s, uint32_t *cp) {
    const uint8_t c = (uint8_t)s[0];
    if (c < 0x80) return *cp = c, 1;
    if ((c & 0xE0) == 0xC0 && s[1]) return *cp = ((c & 0x1F) << 6) | (s[1] & 0x3F), 2;
    if ((c & 0xF0) == 0xE0 && s[1] && s[2]) return *cp = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F), 3;
    if ((c & 0xF8) == 0xF0 && s[1] && s[2] && s[3])
        return *cp = ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F), 4;
    return *cp = '?', 1;
}

static void mask_at(lv_layer_t *L, const lv_draw_buf_t *b, int x0, int y0, uint32_t c, lv_opa_t opa);
static int glyph_index(uint32_t cp) { return cp >= '0' && cp <= '9' ? (int)(cp - '0') : cp == '.' ? 10 : -1; }

/** Text centred on (cx, cy), its baseline turned by rot (the letters with it), so the person at angle rot + pi/2
 *  reads it upright. One glyph at a time: lv_draw_letter turns each about its own baseline centre. */
static void text(lv_layer_t *L, const char *s, float cx, float cy, int size, uint32_t c, lv_opa_t opa, float rot) {
    const lv_font_t *font = cjk_font_get(size);
    if (!font || !s || opa < 2) return;
    uint32_t cps[40];
    float w[40];
    int n = 0;
    for (const char *p = s; *p && n < 40;) p += utf8(p, &cps[n++]);
    const float ext = (n * 0.5f + 1) * size;  // more than the whole line can reach, turned any way
    if (!on(L, cx - ext, cy - ext, cx + ext, cy + ext)) return;
    LV_PROFILER_BEGIN_TAG("pp_text");
    float total = 0;
    for (int i = 0; i < n; i++) total += w[i] = lv_font_get_glyph_width(font, cps[i], i + 1 < n ? cps[i + 1] : 0);
    const float ux = cosf(rot), uy = sinf(rot), vx = -uy, vy = ux, base = size * 0.36f;
    int32_t deg10 = ri(rot * 1800 / PP_PI) % 3600;
    if (deg10 < 0) deg10 += 3600;
    lv_draw_letter_dsc_t d;
    lv_draw_letter_dsc_init(&d);
    d.font = font;
    d.color = col(c);
    d.opa = opa;
    d.rotation = deg10;
    const bool masks = size == GLYPH_SIZE && fabsf(s_glyph_rot - rot) < 1e-4f;
    float x = -total / 2;
    for (int i = 0; i < n; i++) {
        const float m = x + w[i] / 2;
        d.unicode = cps[i];
        lv_point_t pt = {ri(cx + ux * m + vx * base), ri(cy + uy * m + vy * base)};
        const int g = masks ? glyph_index(cps[i]) : -1;
        if (g >= 0 && s_glyph[g])  // its mask, placed from the point lv_draw_letter turns it about (pt)
            mask_at(L, s_glyph[g], pt.x + s_glyph_off[g].x, pt.y + s_glyph_off[g].y, c, opa);
        else if (on(L, pt.x - size, pt.y - size, pt.x + size, pt.y + size))
            lv_draw_letter(L, &d, &pt);
        x += w[i];
    }
    LV_PROFILER_END_TAG("pp_text");
}

/** Text in the facing frame at (x, y) of that frame. */
static void ftext(lv_layer_t *L, const char *s, float x, float y, int size, uint32_t c, lv_opa_t opa) {
    float sx, sy;
    to_screen(P->rot, x, y, &sx, &sy);
    text(L, s, sx, sy, size, c, opa, P->rot);
}

static const int8_t PIPS[9][9][2] = {
    {{0}},
    {{0, 0}},
    {{-1, -1}, {1, 1}},
    {{-1, -1}, {0, 0}, {1, 1}},
    {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}},
    {{-1, -1}, {1, -1}, {0, 0}, {-1, 1}, {1, 1}},
    {{-1, -1}, {1, -1}, {-1, 0}, {1, 0}, {-1, 1}, {1, 1}},
    {{-1, -1}, {1, -1}, {-1, 0}, {1, 0}, {-1, 1}, {1, 1}, {0, 0}},
    {{-1, -1}, {1, -1}, {-1, 0}, {1, 0}, {-1, 1}, {1, 1}, {0, -1}, {0, 1}},
};

/** A seat's badge: its colour with 1-8 dots, so seats are told apart without relying on colour alone. */
static void badge(lv_layer_t *L, float x, float y, int n, uint32_t c, float r, uint32_t pip, float rot) {
    disc(L, x, y, r, c, LV_OPA_COVER);
    n = pp_clampi(n, 1, 8);
    const float off = r * 0.44f, pr = r * 0.16f, cs = cosf(rot), sn = sinf(rot);
    for (int i = 0; i < n; i++) {
        const float px = PIPS[n][i][0] * off, py = PIPS[n][i][1] * off;
        disc(L, x + px * cs - py * sn, y + px * sn + py * cs, pr, pip, 220);
    }
}

static void die(lv_layer_t *L, float x, float y, float size, float rot, int face, float sx) {
    if (face < 1 || face > 6 || !s_face[face]) return;
    const float e = size * 0.75f * fmaxf(1, sx) + 6;  // the turned face's corners, and its shadow
    if (!on(L, x - e, y - e, x + e, y + e)) return;
    LV_PROFILER_BEGIN_TAG("pp_die");
    disc(L, x + 3, y + 5, size * 0.52f, 0x000000, 70);  // its shadow on the glass
    lv_draw_image_dsc_t d;
    lv_draw_image_dsc_init(&d);
    veil_image(&d);
    d.src = s_face[face];
    int32_t r10 = ri(rot * 1800 / PP_PI) % 3600;
    if (r10 < 0) r10 += 3600;
    d.rotation = r10;
    const float k = size / FACE_PX;
    d.scale_x = ri(256 * k * sx);
    d.scale_y = ri(256 * k);
    d.pivot = (lv_point_t){FACE_PX / 2, FACE_PX / 2};
    d.antialias = 1;
    const lv_area_t a = {ri(x) - FACE_PX / 2, ri(y) - FACE_PX / 2, ri(x) - FACE_PX / 2 + FACE_PX - 1, ri(y) - FACE_PX / 2 + FACE_PX - 1};
    lv_draw_image(L, &d, &a);
    LV_PROFILER_END_TAG("pp_die");
}

/** The die faces, drawn once into ARGB8888 buffers with antialiased edges and pips (Chinese dice: 1 and 4 red). */
static void make_faces(void) {
    static const float PF[7][6][2] = {
        {{0}}, {{0, 0}}, {{-1, -1}, {1, 1}}, {{-1, -1}, {0, 0}, {1, 1}}, {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}},
        {{-1, -1}, {1, -1}, {0, 0}, {-1, 1}, {1, 1}}, {{-1, -1}, {1, -1}, {-1, 0}, {1, 0}, {-1, 1}, {1, 1}},
    };
    static const int PN[7] = {0, 1, 2, 3, 4, 5, 6};
    const float h = FACE_PX / 2.0f, rad = FACE_PX * 0.2f, half = h - 1.5f;
    for (int f = 1; f <= 6; f++) {
        lv_draw_buf_t *b = lv_draw_buf_create(FACE_PX, FACE_PX, LV_COLOR_FORMAT_ARGB8888, LV_STRIDE_AUTO);
        if (!b) {
            ESP_LOGE(TAG, "no memory for the die faces");
            return;
        }
        const bool red = f == 1 || f == 4;
        const uint32_t pc = red ? PIP_RED : PIP;
        const float pr = f == 1 ? FACE_PX * 0.13f : FACE_PX * 0.085f, off = FACE_PX * 0.27f;
        for (int y = 0; y < FACE_PX; y++) {
            uint8_t *row = b->data + y * b->header.stride;
            for (int x = 0; x < FACE_PX; x++) {
                const float px = x + 0.5f - h, py = y + 0.5f - h;
                // rounded square: signed distance
                const float qx = fabsf(px) - (half - rad), qy = fabsf(py) - (half - rad);
                const float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
                const float dist = sqrtf(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0) - rad;
                float a = pp_clampf(0.5f - dist, 0, 1);
                uint32_t c = DIE_FACE;
                float r = (c >> 16) & 255, g = (c >> 8) & 255, bl = c & 255;
                if (py > half - FACE_PX * 0.12f) r *= 0.92f, g *= 0.92f, bl *= 0.92f;  // a hint of the side below
                float pip = 0;
                for (int i = 0; i < PN[f]; i++) {
                    const float dx = px - PF[f][i][0] * off, dy = py - PF[f][i][1] * off;
                    pip = fmaxf(pip, pp_clampf(pr - sqrtf(dx * dx + dy * dy) + 0.5f, 0, 1));
                }
                r += (((pc >> 16) & 255) - r) * pip;
                g += (((pc >> 8) & 255) - g) * pip;
                bl += ((pc & 255) - bl) * pip;
                row[x * 4 + 0] = (uint8_t)bl;
                row[x * 4 + 1] = (uint8_t)g;
                row[x * 4 + 2] = (uint8_t)r;
                row[x * 4 + 3] = (uint8_t)(a * 255);
            }
        }
        s_face[f] = b;
    }
}

// ───────────────────────── Yay (yaykit_ui's yk_yay: blinks by itself, poses crossfade, turns to any angle)
static yk_yay_pose_t yay_pose(pp_pose_t p) {
    switch (p) {
    case PP_POSE_DONE: return YK_YAY_DONE;
    case PP_POSE_HELP: return YK_YAY_HELP;
    case PP_POSE_REMINDER: return YK_YAY_REMINDER;
    case PP_POSE_RESTING: return YK_YAY_RESTING;
    case PP_POSE_SPEAKING: return YK_YAY_SPEAKING;
    default: return YK_YAY_GREET;
    }
}

static void yay_update(void) {
    if (!s_yay || P->yay.serial == s_yay_serial) return;
    s_yay_serial = P->yay.serial;
    lv_obj_t *obj = yk_yay_obj(s_yay);
    if (P->yay.mode == PP_YAY_HIDDEN) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    const bool host = P->yay.mode == PP_YAY_HOST;
    float x = PP_C, y = PP_C + 8;
    if (!host) to_screen(P->yay.rot, P->yay.ox, P->yay.oy, &x, &y);
    const bool was_hidden = lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
    yk_yay_size(s_yay, host ? 176 : 132);  // hosting 176, peeking 132 (round-screen-design.md 7)
    yk_yay_move(s_yay, ri(x), ri(y));
    yk_yay_rotate(s_yay, P->yay.rot * 180 / PP_PI, 0);
    yk_yay_pose(s_yay, yay_pose(P->yay.pose), was_hidden ? 0 : 120);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

// ───────────────────────── scenes
static const char *T(const char *zh, const char *en) { return P->en ? en : zh; }
static uint32_t seat_color(int i) { return PP_SEAT_COLORS[P->seats.s[i].color]; }

static void game_icon(lv_layer_t *L, int game, float cx, float cy, float s, float rot, lv_opa_t opa) {
    const float c = cosf(rot), sn = sinf(rot);
#define AT(px, py) cx + (px) * c - (py) * sn, cy + (px) * sn + (py) * c
    switch (game) {
    case PP_GAME_DICE: {
        float x, y;
        x = cx + (-0.32f * s) * c - (0.12f * s) * sn, y = cy + (-0.32f * s) * sn + (0.12f * s) * c;
        die(L, x, y, s * 0.95f, rot - 0.3f, 1, 1);
        x = cx + (0.36f * s) * c - (-0.22f * s) * sn, y = cy + (0.36f * s) * sn + (-0.22f * s) * c;
        die(L, x, y, s * 0.95f, rot + 0.22f, 5, 1);
        break;
    }
    case PP_GAME_WHEEL: {
        static const uint32_t W4[4] = {0xDF795D, 0xF3E6C8, 0x9FB38C, 0xE3A857};
        for (int i = 0; i < 4; i++)
            arc(L, cx, cy, s, s * 0.38f, rot + i * PP_PI / 2 - PP_PI / 4 + 0.06f, rot + (i + 1) * PP_PI / 2 - PP_PI / 4 - 0.06f, W4[i], opa, false);
        const float a = rot - PP_PI / 2 + sinf(P->now * 1.3f) * 0.5f, len = s * 0.6f, wd = s * 0.16f;
        tri(L, cx + cosf(a) * len, cy + sinf(a) * len, cx - sinf(a) * wd, cy + cosf(a) * wd, cx + sinf(a) * wd, cy - cosf(a) * wd, CREAM, opa);
        break;
    }
    case PP_GAME_REACT:
        for (int i = 0; i < 8; i++) {
            const float a = rot + i * PP_TAU / 8 + 0.2f;
            line(L, cx + cosf(a) * s * 0.55f, cy + sinf(a) * s * 0.55f, cx + cosf(a) * s * 0.95f, cy + sinf(a) * s * 0.95f, s * 0.1f, AMBER, opa);
        }
        disc(L, cx, cy, s * 0.36f, AMBER, opa);
        disc(L, cx, cy, s * 0.14f, PAPER, opa);
        break;
    case PP_GAME_BOMB: {
        float bx, by;
        to_screen(0, 0, 0, &bx, &by);
        disc(L, AT(0, s * 0.08f), s * 0.72f, BOMB, opa);
        arc(L, AT(0, s * 0.08f), s * 0.72f, 2, 0, PP_TAU, BOMB_RIM, opa, false);
        float x0, y0, x1, y1;
        x0 = cx + (0.4f * s) * c - (-0.5f * s) * sn, y0 = cy + (0.4f * s) * sn + (-0.5f * s) * c;
        x1 = cx + (0.7f * s) * c - (-0.9f * s) * sn, y1 = cy + (0.7f * s) * sn + (-0.9f * s) * c;
        line(L, x0, y0, x1, y1, s * 0.08f, CREAM, opa);
        disc(L, x1, y1, s * 0.12f, AMBER, opa);
        (void)bx, (void)by;
        break;
    }
    case PP_GAME_TEN: {
        float x, y;
        to_screen(rot, 0, 0, &x, &y);
        arc(L, cx, cy, s * 0.82f, s * 0.08f, 0, PP_TAU, CREAM, opa, false);
        arc(L, cx, cy, s * 0.66f, s * 0.12f, rot - PP_PI / 2, rot - PP_PI / 2 + PP_TAU * 0.72f, CORAL, opa, true);
        text(L, "10", cx, cy, s < 30 ? 18 : 34, INK, opa, rot);
        (void)x, (void)y;
        break;
    }
    default: break;
    }
#undef AT
}

static const char *const GAME_ZH[5] = {"摇骰子", "幸运转盘", "抢拍", "传炸弹", "十秒挑战"};
static const char *const GAME_EN[5] = {"Dice", "Spinner", "Snap", "Hot Bomb", "Ten Seconds"};

static int menu_sel(void) {
    int s = (int)lroundf(P->menu_pos) % 5;
    return s < 0 ? s + 5 : s;
}

static void icon_pos(int i, float *x, float *y) {
    const float a = P->rot - PP_PI / 2 + (i - P->menu_pos) * PP_TAU / 5;
    *x = PP_C + 178 * cosf(a);
    *y = PP_C + 178 * sinf(a);
}

static uint32_t fnv(uint32_t h, int v);
static uint32_t center_key(void) { return fnv(fnv(2166136261u, (int)lroundf(P->rot * 1000)), P->en) | 1; }

static void menu_center(lv_layer_t *L, int sel) {
    float x, y;
    to_screen(P->rot, 0, -32, &x, &y);
    game_icon(L, sel, x, y, 44, P->rot, LV_OPA_COVER);
    ftext(L, T(GAME_ZH[sel], GAME_EN[sel]), 0, 46, 34, INK, LV_OPA_COVER);
    ftext(L, T("轻触开始", "Tap to play"), 0, 88, 18, INK2, LV_OPA_COVER);
}

static void draw_menu(lv_layer_t *L) {
    if (BG) arc(L, PP_C, PP_C, 216, 76, 0, PP_TAU, PANEL, 90, false);
    if (!FG) return;
    for (int i = 0; i < 5; i++) {
        float x, y;
        icon_pos(i, &x, &y);
        const float d = fabsf(pp_ang_n((i - P->menu_pos) * PP_TAU / 5));
        const float near = 1 - pp_clampf(d / (PP_TAU / 5), 0, 1);
        disc(L, x, y, 30 + near * 6, PAPER, LV_OPA_COVER);
        if (near > 0.5f) arc(L, x, y, 38, 3, 0, PP_TAU, SAGE, (lv_opa_t)((near - 0.5f) * 2 * 255), false);
        const int k = (int)lroundf(near * (ICON_SIZES - 1));
        const lv_opa_t opa = (lv_opa_t)(140 + near * 115);
        if (!on(L, x - ICON_PX / 2, y - ICON_PX / 2, x + ICON_PX / 2, y + ICON_PX / 2)) {
            // outside this band: nothing of it to draw
        } else if (s_icon[i][k] && fabsf(s_icon_rot - P->rot) < 1e-4f) {
            lv_draw_image_dsc_t d;
            lv_draw_image_dsc_init(&d);
            veil_image(&d);
            d.src = s_icon[i][k];
            d.opa = opa;
            const lv_area_t a = {ri(x) - ICON_PX / 2, ri(y) - ICON_PX / 2, ri(x) - ICON_PX / 2 + ICON_PX - 1, ri(y) - ICON_PX / 2 + ICON_PX - 1};
            lv_draw_image(L, &d, &a);
        } else {
            game_icon(L, i, x, y, 19 + near * 4, P->rot, opa);
        }
    }
    const int sel = menu_sel(), c0 = PP_C - CENTER_PX / 2;
    if (!on(L, c0, c0, c0 + CENTER_PX, c0 + CENTER_PX)) return;
    if (s_center[sel] && s_center_key[sel] == center_key()) {
        lv_draw_image_dsc_t d;
        lv_draw_image_dsc_init(&d);
        veil_image(&d);
        d.src = s_center[sel];
        const lv_area_t a = {c0, c0, c0 + CENTER_PX - 1, c0 + CENTER_PX - 1};
        lv_draw_image(L, &d, &a);
    } else {
        menu_center(L, sel);
    }
}

static void draw_seats(lv_layer_t *L) {
    const bool coloring = P->color_at >= 0;
    if (BG) {
        for (int k = 0; k < 90; k++) {  // a dotted circle where the seats go
            const float a = k * PP_TAU / 90;
            disc(L, PP_C + 206 * cosf(a), PP_C + 206 * sinf(a), 1.6f, INK3, 200);
        }
        if (!coloring)
            for (int k = 0; k < 3; k++) {  // the hint, three times round so everyone has one the right way up
                const float a = P->rot + PP_PI / 2 + k * PP_TAU / 3;
                text(L, T("点你面前的边", "Tap your edge"), PP_C + 166 * cosf(a), PP_C + 166 * sinf(a), 18, INK2, 230, a - PP_PI / 2);
            }
    }
    if (!FG) return;
    const pp_seats_t *s = coloring ? &P->seats : &P->seating;
    for (int i = 0; i < s->n; i++) {
        const float x = PP_C + 206 * cosf(s->s[i].a), y = PP_C + 206 * sinf(s->s[i].a);
        if (coloring && i < P->color_at) {
            badge(L, x, y, i + 1, PP_SEAT_COLORS[s->s[i].color], 15, PAPER, s->s[i].a - PP_PI / 2);
        } else {
            const float age = coloring ? 1 : (P->now - P->seat_born[i]) / 0.25f;
            disc(L, x, y, 12 * pp_ease_back(age), CREAM, LV_OPA_COVER);
        }
    }
    const int n = s->n;
    char num[4];
    lv_snprintf(num, sizeof(num), "%d", n ? n : P->players);
    ftext(L, num, 0, -18, 88, n ? INK : INK3, LV_OPA_COVER);
    float bx, by;
    to_screen(P->rot, 0, 58, &bx, &by);
    disc(L, bx, by, 28, n == 1 ? PANEL : CORAL, LV_OPA_COVER);
    float x0, y0, x1, y1, x2, y2;
    to_screen(P->rot, -8, 46, &x0, &y0);
    to_screen(P->rot, 13, 58, &x1, &y1);
    to_screen(P->rot, -8, 70, &x2, &y2);
    tri(L, x0, y0, x1, y1, x2, y2, INK, LV_OPA_COVER);
}

static void draw_dice(lv_layer_t *L) {
    const pp_dice_t *d = &P->dice;
    for (int i = 0; i < d->n; i++) {
        const pp_die_t *e = &d->d[i];
        const float sx = 1 - 0.38f * sinf(PP_PI * e->tum);
        die(L, PP_C + e->x, PP_C + e->y, PP_DIE_PX * P->scale[i], e->a, e->face, sx);
        if (P->dice_st == PP_DICE_RESULT && P->dice_special) {
            const float r = PP_DIE_PX * P->scale[i] * 0.62f;
            arc(L, PP_C + e->x, PP_C + e->y, r + 4, 4, 0, PP_TAU, SAGE, 200, false);
        }
    }
    if (P->dice_st == PP_DICE_RESULT && d->n > 1) {
        char num[4];
        lv_snprintf(num, sizeof(num), "%d", pp_dice_sum(d));
        ftext(L, num, 0, 52, 88, INK, LV_OPA_COVER);
    }
    if (P->dice_st == PP_DICE_IDLE) ftext(L, T("摇一摇或轻触", "Shake or tap"), 0, 150, 18, INK2, LV_OPA_COVER);
}

static void needle(lv_layer_t *L, float th) {
    const float len = 138, wd = 18, c = cosf(th), s = sinf(th);
    const float tipx = PP_C + c * len, tipy = PP_C + s * len;
    const float lx = PP_C - s * wd, ly = PP_C + c * wd, rx = PP_C + s * wd, ry = PP_C - c * wd;
    const float tx = PP_C - c * len * 0.22f, ty = PP_C - s * len * 0.22f;
    tri(L, tipx, tipy, lx, ly, rx, ry, CREAM, LV_OPA_COVER);
    tri(L, tx, ty, lx, ly, rx, ry, CREAM, LV_OPA_COVER);
    const float bx = PP_C + c * len * 0.72f, by = PP_C + s * len * 0.72f;
    tri(L, tipx, tipy, bx - s * wd * 0.42f, by + c * wd * 0.42f, bx + s * wd * 0.42f, by - c * wd * 0.42f, CORAL, LV_OPA_COVER);
}

static void draw_wheel(lv_layer_t *L) {
    const int n = P->seats.n;
    for (int i = 0; i < n; i++) {
        float lo, hi;
        pp_seats_sector(&P->seats, i, &lo, &hi);
        const bool win = P->wheel_st == PP_WHEEL_RESULT && i == P->wheel.winner;
        const float x = PP_C + 186 * cosf(P->seats.s[i].a), y = PP_C + 186 * sinf(P->seats.s[i].a);
        if (win ? FG : BG) {  // the winner's slice and seat beat: each frame, over a background without them
            const float pulse = win ? 0.5f + 0.5f * pp_play_pulse(P) : 0.2f;
            band(L, 150, 222, lo + 0.025f, hi - 0.025f, seat_color(i), (lv_opa_t)(pulse * 255));
            badge(L, x, y, i + 1, win ? PAPER : seat_color(i), 14, win ? seat_color(i) : PAPER, P->seats.s[i].a - PP_PI / 2);
        } else if (FG && P->flash[i] > 0) {  // the slice under the needle lights up over its still self: 0.2 + 0.45 flash
            band(L, 150, 222, lo + 0.025f, hi - 0.025f, seat_color(i), (lv_opa_t)(P->flash[i] * (0.45f / 0.8f) * 255));
            badge(L, x, y, i + 1, seat_color(i), 14, PAPER, P->seats.s[i].a - PP_PI / 2);
        }
    }
    if (BG && P->wheel_st == PP_WHEEL_RESULT) arc(L, PP_C, PP_C, 141, 3, 0, PP_TAU, seat_color(P->wheel.winner), 128, false);
    if (!FG) return;
    needle(L, P->wheel.th);
    disc(L, PP_C, PP_C, 22, PANEL, LV_OPA_COVER);
    arc(L, PP_C, PP_C, 23, 3, 0, PP_TAU, CREAM, LV_OPA_COVER, false);
    if (P->wheel_st == PP_WHEEL_IDLE && P->spins == 0) ftext(L, T("拨一下指针", "Flick the needle"), 0, 98, 18, INK2, LV_OPA_COVER);
}

static void draw_react(lv_layer_t *L) {
    const pp_react_t *r = &P->react;
    const bool go = r->st == PP_RX_GO, decoy = r->st == PP_RX_DECOY;
    if (go || decoy) fill_screen(L, go ? AMBER : SAGE, LV_OPA_COVER);
    for (int i = 0; i < P->seats.n; i++) {
        float lo, hi;
        pp_seats_sector(&P->seats, i, &lo, &hi);
        const uint32_t c = seat_color(i);
        const bool lit = ((r->st == PP_RX_WIN || r->st == PP_RX_OVER) && i == r->winner) ||
                         (r->st == PP_RX_TIE && (i == P->tie_a || i == P->tie_b));
        const bool pulsing = r->st == PP_RX_OVER && i == r->winner;  // the winner's slice beats: drawn each frame
        if (go || decoy) {
            line(L, PP_C + 96 * cosf(lo), PP_C + 96 * sinf(lo), PP_C + 233 * cosf(lo), PP_C + 233 * sinf(lo), 4, PAPER, LV_OPA_COVER);
        } else if (pulsing ? FG : BG) {
            const float o = lit ? (pulsing ? 0.5f + 0.5f * pp_play_pulse(P) : 0.95f) : r->locked[i] ? 0.08f : 0.18f;
            band(L, 100, 228, lo + 0.02f, hi - 0.02f, c, (lv_opa_t)(o * 255));
        }
        if (pulsing ? !FG : !BG) continue;  // the beating seat, all of it over its slice, each frame
        const float a = P->seats.s[i].a, ra = a - PP_PI / 2;
        badge(L, PP_C + 200 * cosf(a), PP_C + 200 * sinf(a), i + 1, go || decoy || lit ? PAPER : c, 14,
              go ? AMBER : decoy ? SAGE : lit ? c : PAPER, ra);
        for (int k = 0; k < PP_REACT_TARGET; k++) {
            const float b = a + (k - 1) * 0.13f, x = PP_C + 128 * cosf(b), y = PP_C + 128 * sinf(b);
            if (k < r->score[i]) disc(L, x, y, 5.5f, go || decoy || lit ? PAPER : c, LV_OPA_COVER);
            else arc(L, x, y, 6.5f, 2, 0, PP_TAU, go || decoy ? PAPER : INK3, go || decoy ? 128 : LV_OPA_COVER, false);
        }
        if (r->locked[i] && !go && !decoy) {
            const float x = PP_C + 160 * cosf(a), y = PP_C + 160 * sinf(a), c1 = cosf(ra), s1 = sinf(ra);
            line(L, x + (-12 * c1 + 12 * s1), y + (-12 * s1 - 12 * c1), x + (12 * c1 - 12 * s1), y + (12 * s1 + 12 * c1), 6, CORAL, LV_OPA_COVER);
            line(L, x + (12 * c1 + 12 * s1), y + (12 * s1 - 12 * c1), x + (-12 * c1 - 12 * s1), y + (-12 * s1 + 12 * c1), 6, CORAL, LV_OPA_COVER);
        }
    }
    if (r->st == PP_RX_WAIT && FG) {
        const float k = 0.5f + 0.5f * sinf(P->now * 6.8f);
        arc(L, PP_C, PP_C, 42 + k * 10, 4, 0, PP_TAU, INK3, (lv_opa_t)((0.5f + k * 0.4f) * 255), false);
    }
    if (r->st == PP_RX_OVER && FG) {
        const float k = pp_clampf(r->t / 0.6f, 0, 1), fade = pp_clampf(2.2f - r->t, 0, 1);
        arc(L, PP_C, PP_C, 229, 6, -PP_PI / 2, -PP_PI / 2 + PP_TAU * k, SAGE, (lv_opa_t)(fade * 255), true);
    }
    if (!BG) return;
    if (r->st == PP_RX_READY) {
        float x, y, x0, y0, x1, y1, x2, y2;
        to_screen(P->rot, 0, 0, &x, &y);
        disc(L, x, y, 46, CORAL, LV_OPA_COVER);
        to_screen(P->rot, -12, -18, &x0, &y0);
        to_screen(P->rot, 20, 0, &x1, &y1);
        to_screen(P->rot, -12, 18, &x2, &y2);
        tri(L, x0, y0, x1, y1, x2, y2, INK, LV_OPA_COVER);
    }
    if (r->st == PP_RX_WIN) {
        char num[8];
        lv_snprintf(num, sizeof(num), "%d.%02d", (int)r->rt, (int)lroundf(r->rt * 100) % 100);
        ftext(L, num, 0, -6, 88, INK, LV_OPA_COVER);
        ftext(L, T("秒", "sec"), 0, 50, 24, INK2, LV_OPA_COVER);
    }
    if (r->st == PP_RX_TIE) ftext(L, T("同时", "Tie"), 0, 0, 56, INK, LV_OPA_COVER);
}

static void fuse_tip(float *x, float *y) {
    const float p = P->bomb.st == PP_BOMB_LIT ? P->bomb.ramp : 0, r = 82, L = r * 0.9f * (1 - p * 0.85f);
    const float tx = sinf(0.6f) * r * 1.05f, ty = 14 - cosf(0.6f) * r * 1.05f;
    to_screen(P->rot, tx + L * 0.7f, ty - L * 0.75f, x, y);
}

static void draw_bomb(lv_layer_t *L) {
    const pp_bomb_t *b = &P->bomb;
    if (b->st == PP_BOMB_BOOM) {
        const float k = pp_clampf(1 - b->t / 0.55f, 0, 1);
        if (k > 0) fill_screen(L, CREAM, (lv_opa_t)(k * 255));
        if (b->t > 0.15f && b->t < 0.9f) arc(L, PP_C, PP_C, 229, 6, 0, PP_TAU, CORAL, (lv_opa_t)((1 - (b->t - 0.15f) / 0.75f) * 255), false);
        return;
    }
    if (b->st == PP_BOMB_PENALTY) {
        const pp_penalty_t *pen = &PP_PENALTIES[b->penalty >= 0 ? b->penalty : 0];
        ftext(L, P->en ? pen->en_short : pen->zh_short, 0, -20, P->en ? 34 : 34, CREAM, (lv_opa_t)(pp_clampf(b->t / 0.25f, 0, 1) * 255));
        arc(L, PP_C, PP_C, 229, 6, 0, PP_TAU, CORAL, 128, false);
        return;
    }
    if (b->st == PP_BOMB_AFTER) {
        ftext(L, T("再来一局", "Again"), 0, 120, 18, INK2, LV_OPA_COVER);
        arc(L, PP_C, PP_C, 61, 2, 0, PP_TAU, INK3, 150, false);
        return;
    }
    const bool urge = b->st == PP_BOMB_LIT && b->urging;
    if (urge) arc(L, PP_C, PP_C, 229, 6, 0, PP_TAU, AMBER, (lv_opa_t)((0.45f + 0.45f * sinf(P->now * 12)) * 255), false);
    const float pop = b->st == PP_BOMB_IDLE ? pp_ease_back(b->t / 0.3f) : 1;
    const float wob = urge ? sinf(P->now * 30) * 4 : 0;
    float cx, cy;
    to_screen(P->rot, wob, 14, &cx, &cy);
    const float r = 82 * pop;
    disc(L, cx, cy, r, BOMB, LV_OPA_COVER);
    arc(L, cx, cy, r, 4, 0, PP_TAU, BOMB_RIM, LV_OPA_COVER, false);
    float hx, hy;
    to_screen(P->rot, wob - r * 0.38f, 14 - r * 0.4f, &hx, &hy);
    disc(L, hx, hy, r * 0.14f, CREAM, 70);
    // the cap and the fuse
    float c0x, c0y, tipx, tipy;
    to_screen(P->rot, wob + sinf(0.6f) * r * 1.05f, 14 - cosf(0.6f) * r * 1.05f, &c0x, &c0y);
    disc(L, c0x, c0y, r * 0.16f, 0x4C564D, LV_OPA_COVER);
    fuse_tip(&tipx, &tipy);
    if (pop >= 1) line(L, c0x, c0y, tipx, tipy, 6, CREAM, LV_OPA_COVER);
    if (b->st == PP_BOMB_LIT) disc(L, tipx, tipy, 7 + 3 * sinf(P->now * 40), AMBER, 220);
    if (b->st == PP_BOMB_IDLE) ftext(L, T("轻触点火", "Tap to light"), 0, 136, 18, INK2, LV_OPA_COVER);
}

static void fmt_secs(char *out, size_t n, float s) {
    const int cs = (int)lroundf(s * 100);
    lv_snprintf(out, n, "%d.%02d", cs / 100, cs % 100);
}

/** How much of ten seconds' number and ring shows, in twelfths: a fade in steps, so the ring and the number are drawn
 *  again a dozen times over its second, not every frame. */
static float ten_shown(const pp_ten_t *t) { return ceilf(pp_ten_visible(t) * 12) / 12; }

static void draw_ten(lv_layer_t *L) {
    const pp_ten_t *t = &P->ten;
    char num[12];
    if (t->multi)
        for (int i = 0; i < P->seats.n; i++) {
            const bool cur = i == t->turn && t->st != PP_TEN_FINAL;
            float lo, hi;
            pp_seats_sector(&P->seats, i, &lo, &hi);
            if (cur) band(L, 206, 222, lo + 0.03f, hi - 0.03f, seat_color(i), 216);
            if (t->st == PP_TEN_FINAL && i == t->winner) band(L, 160, 222, lo + 0.03f, hi - 0.03f, seat_color(i), (lv_opa_t)((0.4f + 0.6f * pp_play_pulse(P)) * 255));
            const float a = P->seats.s[i].a;
            badge(L, PP_C + 188 * cosf(a), PP_C + 188 * sinf(a), i + 1, seat_color(i), 13, PAPER, a - PP_PI / 2);
            if (t->has[i]) {
                fmt_secs(num, sizeof(num), t->results[i]);
                text(L, num, PP_C + 152 * cosf(a), PP_C + 152 * sinf(a), 18, t->st == PP_TEN_FINAL && i == t->winner ? INK : INK2, LV_OPA_COVER, a - PP_PI / 2);
            }
        }
    if (t->st == PP_TEN_IDLE) {
        if (!t->multi) arc(L, PP_C, PP_C, 229, 6, 0, PP_TAU, CORAL, 230, false);
        ftext(L, "10.00", 0, -10, 88, INK, LV_OPA_COVER);
        ftext(L, t->mode ? T("全程盲猜", "Fully blind") : T("前 3 秒看得见", "Visible for 3 s"), 0, 58, 18, INK2, LV_OPA_COVER);
        if (!t->multi && t->best > 0) {
            char b[24];
            fmt_secs(num, sizeof(num), t->best);
            lv_snprintf(b, sizeof(b), "%s%s", T("最好 ", "Best "), num);
            ftext(L, b, 0, 92, 18, INK3, LV_OPA_COVER);
        }
    } else if (t->st == PP_TEN_RUN) {
        const float v = ten_shown(t);
        if (v > 0.01f) {
            arc(L, PP_C, PP_C, 229, 6, -PP_PI / 2 + PP_TAU * pp_clampf(t->t / 10, 0, 1), -PP_PI / 2 + PP_TAU, CORAL, (lv_opa_t)(v * 255), false);
            fmt_secs(num, sizeof(num), t->t);
            ftext(L, num, 0, -10, 88, INK, (lv_opa_t)(v * 255));
        } else {
            const float k = 0.5f + 0.5f * sinf(P->now * 4);
            float x, y;
            to_screen(P->rot, 0, -10, &x, &y);
            disc(L, x, y, 7, INK3, (lv_opa_t)((0.4f + 0.5f * k) * 255));
        }
    } else {
        const float res = t->st == PP_TEN_FINAL && t->winner >= 0 ? t->results[t->winner] : t->res;
        const float d = res - 10, ad = fabsf(d);
        const uint32_t c = ad <= 0.2f ? SAGE_TEXT : ad <= 0.5f ? AMBER : CORAL_TEXT;
        if (!t->multi) {
            const float k = pp_clampf(t->shown_t / 0.5f, 0, 1);
            arc(L, PP_C, PP_C, 229, 6, -PP_PI / 2, -PP_PI / 2 + PP_TAU * fminf(1, res / 10) * k, SAGE, LV_OPA_COVER, false);
            if (res > 10) arc(L, PP_C, PP_C, 229, 6, -PP_PI / 2, -PP_PI / 2 + PP_TAU * fminf(1, (res - 10) / 10) * k, CORAL, LV_OPA_COVER, false);
        }
        fmt_secs(num, sizeof(num), res);
        ftext(L, num, 0, -10, 88, INK, LV_OPA_COVER);
        char dl[16];
        fmt_secs(num, sizeof(num), ad);
        lv_snprintf(dl, sizeof(dl), "%s%s", d >= 0 ? "+" : "−", num);
        ftext(L, dl, 0, 58, 24, c, LV_OPA_COVER);
    }
}

static void draw_settings(lv_layer_t *L) {
    const int lo[4] = {2, 1, 0, 0}, hi[4] = {8, 3, 10, 1};
    const int val[4] = {P->players, P->dice_n, P->volume, P->penalties ? 1 : 0};
    const int sel = P->set_sel, n = hi[sel] - lo[sel];
    for (int i = 0; i <= n; i++) {
        const float a = -PP_PI / 2 + P->rot - 1.0f + 2.0f * i / n;
        line(L, PP_C + 210 * cosf(a), PP_C + 210 * sinf(a), PP_C + 224 * cosf(a), PP_C + 224 * sinf(a), 6, lo[sel] + i <= val[sel] ? SAGE : PANEL, LV_OPA_COVER);
    }
    for (int i = 0; i < 4; i++) {
        float x, y;
        to_screen(P->rot, (i - 1.5f) * 92, -16, &x, &y);
        const bool on = i == sel;
        disc(L, x, y, 38, on ? SAGE_DEEP : PANEL, LV_OPA_COVER);
        if (on) arc(L, x, y, 42, 3, 0, PP_TAU, SAGE, LV_OPA_COVER, false);
        const float r = P->rot, c = cosf(r), s = sinf(r);
#define LOC(px, py) x + (px) * c - (py) * s, y + (px) * s + (py) * c
        if (i == 0) {
            disc(L, LOC(-11, -8), 8, CORAL, LV_OPA_COVER);
            disc(L, LOC(11, -8), 8, CREAM, LV_OPA_COVER);
            arc(L, LOC(-11, 16), 13, 13, r + PP_PI, r + PP_TAU, CORAL, LV_OPA_COVER, false);
            arc(L, LOC(11, 16), 13, 13, r + PP_PI, r + PP_TAU, CREAM, LV_OPA_COVER, false);
        } else if (i == 1) {
            die(L, x, y, 40, r - 0.15f, 3, 1);
        } else if (i == 2) {
            float ax, ay, bx, by, cx2, cy2;
            to_screen(0, 0, 0, &ax, &ay);
            (void)ax, (void)ay;
            ax = x + (-14) * c - (-7) * s, ay = y + (-14) * s + (-7) * c;
            bx = x + (4) * c - (-17) * s, by = y + (4) * s + (-17) * c;
            cx2 = x + (4) * c - (17) * s, cy2 = y + (4) * s + (17) * c;
            tri(L, ax, ay, bx, by, cx2, cy2, CREAM, LV_OPA_COVER);
            arc(L, LOC(4, 0), 12, 3, r - 0.7f, r + 0.7f, CREAM, LV_OPA_COVER, true);
            arc(L, LOC(4, 0), 20, 3, r - 0.7f, r + 0.7f, CREAM, LV_OPA_COVER, true);
        } else {
            arc(L, x, y, 18, 3, 0, PP_TAU, CREAM, LV_OPA_COVER, false);
            disc(L, LOC(-6, -4), 2.6f, CREAM, LV_OPA_COVER);
            disc(L, LOC(6, -4), 2.6f, CREAM, LV_OPA_COVER);
            arc(L, LOC(0, 4), 7, 7, r, r + PP_PI, CORAL, LV_OPA_COVER, false);
        }
#undef LOC
        float vx, vy;
        to_screen(P->rot, (i - 1.5f) * 92, 58, &vx, &vy);
        if (i == 3) {
            if (P->penalties) disc(L, vx, vy, 9, SAGE, LV_OPA_COVER);
            else arc(L, vx, vy, 10, 3, 0, PP_TAU, INK3, LV_OPA_COVER, false);
        } else {
            char num[4];
            lv_snprintf(num, sizeof(num), "%d", val[i]);
            text(L, num, vx, vy, 34, on ? INK : INK2, LV_OPA_COVER, P->rot);
        }
    }
}

static void cup(lv_layer_t *L, float x, float y, uint32_t c) {
    const float r = P->rot, cs = cosf(r), sn = sinf(r);
#define LOC(px, py) x + (px) * cs - (py) * sn, y + (px) * sn + (py) * cs
    arc(L, LOC(0, -30), 28, 28, r, r + PP_PI, c, LV_OPA_COVER, false);
    line(L, LOC(0, 0), LOC(0, 22), 8, c, LV_OPA_COVER);
    line(L, LOC(-16, 25), LOC(16, 25), 6, c, LV_OPA_COVER);
    arc(L, LOC(-28, -22), 9, 4, r + PP_PI * 0.5f, r + PP_PI * 1.5f, c, LV_OPA_COVER, false);
    arc(L, LOC(28, -22), 9, 4, r - PP_PI * 0.5f, r + PP_PI * 0.5f, c, LV_OPA_COVER, false);
#undef LOC
}

static void draw_scores(lv_layer_t *L) {
    int max = 0;
    for (int i = 0; i < P->seats.n; i++) max = P->wins[i] > max ? P->wins[i] : max;
    if (P->seats.n < 2 || !max) {
        float x, y;
        to_screen(P->rot, 0, -20, &x, &y);
        cup(L, x, y, INK3);
        ftext(L, T("还没开局", "No rounds yet"), 0, 60, 18, INK2, LV_OPA_COVER);
        return;
    }
    for (int i = 0; i < P->seats.n; i++) {
        float lo, hi;
        pp_seats_sector(&P->seats, i, &lo, &hi);
        const int w = P->wins[i];
        const float len = 40 + 120.0f * w / max, a = P->seats.s[i].a;
        band(L, 222 - len * 0.6f, 222, lo + 0.04f, hi - 0.04f, seat_color(i), w == max ? 216 : 90);
        badge(L, PP_C + 120 * cosf(a), PP_C + 120 * sinf(a), i + 1, seat_color(i), 12, PAPER, a - PP_PI / 2);
        char num[6];
        lv_snprintf(num, sizeof(num), "%d", w);
        text(L, num, PP_C + 196 * cosf(a), PP_C + 196 * sinf(a), 24, w == max ? PAPER : INK, LV_OPA_COVER, a - PP_PI / 2);
    }
    cup(L, PP_C, PP_C, AMBER);
}

static void draw_charge(lv_layer_t *L) {
    float x, y;
    to_screen(P->rot, 0, 126, &x, &y);
    arc(L, x, y, 20, 4, 0, PP_TAU, AMBER, LV_OPA_COVER, false);
    disc(L, x - 6, y, 4, AMBER, LV_OPA_COVER);
    ftext(L, T("插上电再玩", "Plug me in"), 0, 172, 18, INK2, LV_OPA_COVER);
}

static void draw_sheet(lv_layer_t *L) {
    float x, y;
    to_screen(P->rot, 0, -233 - 80, &x, &y);
    disc(L, x, y, 233, 0x1F2521, 248);
    char buf[24];
    if (P->battery_pct >= 0) lv_snprintf(buf, sizeof(buf), "%d%%", P->battery_pct);
    else lv_snprintf(buf, sizeof(buf), "—");
    float bx, by;
    to_screen(P->rot, -60, -150, &bx, &by);
    arc(L, bx, by, 14, 3, 0, PP_TAU, P->battery ? AMBER : SAGE, LV_OPA_COVER, false);
    ftext(L, buf, -18, -150, 24, INK, LV_OPA_COVER);
    lv_snprintf(buf, sizeof(buf), "%s %d", T("音量", "Vol"), P->volume);
    ftext(L, buf, 74, -150, 24, INK, LV_OPA_COVER);
}

/** A capsule's coverage (the segment from -h to h about the middle, radius rad; h = 0 a disc), antialiased. */
static lv_draw_buf_t *capsule(float hx, float hy, float rad) {
    lv_draw_buf_t *b = lv_draw_buf_create(SPR_PX, SPR_PX, LV_COLOR_FORMAT_A8, LV_STRIDE_AUTO);
    if (!b) return NULL;
    const float hh = hx * hx + hy * hy;
    for (int y = 0; y < SPR_PX; y++)
        for (int x = 0; x < SPR_PX; x++) {
            const float px = x + 0.5f - SPR_PX / 2.0f, py = y + 0.5f - SPR_PX / 2.0f;
            const float t = hh > 0 ? pp_clampf((px * hx + py * hy) / hh, -1, 1) : 0;
            const float d = hypotf(px - t * hx, py - t * hy);
            b->data[y * b->header.stride + x] = (uint8_t)(pp_clampf(rad - d + 0.5f, 0, 1) * 255);
        }
    return b;
}

static void make_sprites(void) {
    for (int k = 0; k < CONF_SIZES; k++)
        for (int a = 0; a < CONF_ANGLES; a++) {
            const float sz = 0.85f + 0.2f * k, ang = a * PP_PI / CONF_ANGLES;  // sizes 0.85, 1.05, 1.25 of the piece
            s_conf[k][a] = capsule(cosf(ang) * 5 * sz, sinf(ang) * 5 * sz, 2.5f * sz);
        }
    for (int i = 0; i < DOTS; i++) s_dot[i] = capsule(0, 0, 2 + 0.5f * i);
}

/** A coverage mask (A8) with its top left at (x0, y0), in colour c. */
static void mask_at(lv_layer_t *L, const lv_draw_buf_t *b, int x0, int y0, uint32_t c, lv_opa_t opa) {
    const int w = b->header.w, h = b->header.h;
    if (opa < 2 || !on(L, x0, y0, x0 + w, y0 + h)) return;
    lv_draw_image_dsc_t d;
    lv_draw_image_dsc_init(&d);
    d.src = b;
    d.opa = opa;
    d.recolor = col(c);  // an A8 image is drawn in its recolour
    const lv_area_t a = {x0, y0, x0 + w - 1, y0 + h - 1};
    lv_draw_image(L, &d, &a);
}

/** The same centred on (x, y). */
static void sprite(lv_layer_t *L, const lv_draw_buf_t *b, float x, float y, uint32_t c, lv_opa_t opa) {
    mask_at(L, b, ri(x) - b->header.w / 2, ri(y) - b->header.h / 2, c, opa);
}

static void dot(lv_layer_t *L, float x, float y, float r, uint32_t c, lv_opa_t opa) {
    const int i = pp_clampi((int)lroundf((r - 2) * 2), 0, DOTS - 1);
    if (s_dot[i]) sprite(L, s_dot[i], x, y, c, opa);
    else disc(L, x, y, r, c, opa);
}

/** A small particle Yay stands in front of (hosting or peeking): neither drawn nor redrawn there, so Yay's turned
 *  image is not drawn again every frame while confetti flies out from behind it. */
static bool behind_yay(const pp_part_t *q) {
    if (!s_yay || P->yay.mode == PP_YAY_HIDDEN || q->kind == PP_PART_SMOKE) return false;
    const bool host = P->yay.mode == PP_YAY_HOST;
    float cx = PP_C, cy = PP_C + 8;
    if (!host) to_screen(P->yay.rot, P->yay.ox, P->yay.oy, &cx, &cy);
    const float r = host ? 70 : 52;  // inside its outline at 176 and 132
    return (q->x - cx) * (q->x - cx) + (q->y - cy) * (q->y - cy) < r * r;
}

static void draw_parts(lv_layer_t *L) {
    for (int i = 0; i < P->nparts; i++) {
        const pp_part_t *q = &P->parts[i];
        if (behind_yay(q)) continue;
        const lv_opa_t opa = (lv_opa_t)(pp_clampf(q->life / 0.6f, 0, 1) * 255);
        switch (q->kind) {
        case PP_PART_CONFETTI: {
            const int k = pp_clampi((int)lroundf((q->s - 0.85f) / 0.2f), 0, CONF_SIZES - 1);
            int a = (int)lroundf(q->r / PP_PI * CONF_ANGLES) % CONF_ANGLES;
            if (a < 0) a += CONF_ANGLES;
            if (s_conf[k][a]) {
                sprite(L, s_conf[k][a], q->x, q->y, q->color, opa);
            } else {
                const float c = cosf(q->r) * 5 * q->s, s = sinf(q->r) * 5 * q->s;
                line(L, q->x - c, q->y - s, q->x + c, q->y + s, 5 * q->s, q->color, opa);
            }
            break;
        }
        case PP_PART_SPARK: dot(L, q->x, q->y, 2.4f * q->s, q->color, opa); break;
        case PP_PART_DEBRIS: dot(L, q->x, q->y, 4 * q->s, q->color, opa); break;
        case PP_PART_SMOKE: disc(L, q->x, q->y, 16 + (1.5f - q->life) * 40, q->color, (lv_opa_t)(opa * 0.35f)); break;
        }
    }
}

static void scene_draw(lv_layer_t *L) {
    switch (P->scene) {
    case PP_SC_MENU: draw_menu(L); break;
    case PP_SC_SEATS: draw_seats(L); break;
    case PP_SC_DICE: draw_dice(L); break;
    case PP_SC_WHEEL: draw_wheel(L); break;
    case PP_SC_REACT: draw_react(L); break;
    case PP_SC_BOMB: draw_bomb(L); break;
    case PP_SC_TEN: draw_ten(L); break;
    case PP_SC_SETTINGS: draw_settings(L); break;
    case PP_SC_SCORES: draw_scores(L); break;
    case PP_SC_CHARGE: draw_charge(L); break;
    default: break;
    }
}

static uint32_t fnv(uint32_t h, int v);

/** What the scene's still part depends on (0: this scene draws it all every frame). */
static uint32_t bg_key(void) {
    uint32_t h = fnv(fnv(2166136261u, P->scene), s_veil);
    switch (P->scene) {
    case PP_SC_MENU: break;  // the ring: the same whoever it faces
    case PP_SC_WHEEL:
        h = fnv(fnv(fnv(h, P->seats.n), P->wheel_st == PP_WHEEL_RESULT ? P->wheel.winner : -1), P->en);
        break;
    case PP_SC_SEATS:
        h = fnv(fnv(fnv(h, (int)lroundf(P->rot * 1000)), P->en), P->color_at >= 0);
        break;
    case PP_SC_REACT: {
        const pp_react_t *r = &P->react;
        if (r->st == PP_RX_GO || r->st == PP_RX_DECOY) return 0;  // a plain fill and a few marks: cheap as it is
        h = fnv(fnv(fnv(fnv(h, (int)lroundf(P->rot * 1000)), P->en), r->st), r->winner);
        h = fnv(fnv(fnv(fnv(h, P->tie_a), P->tie_b), P->seats.n), (int)lroundf(r->rt * 100));
        for (int i = 0; i < r->n; i++) h = fnv(h, r->score[i] * 2 + r->locked[i]);
        break;
    }
    default: return 0;
    }
    return h | 1;
}

/** Draws the still part into the background buffer (in the LVGL task, under the lock). */
static void bg_render(void) {
    LV_PROFILER_BEGIN_TAG("pp_bg_render");
    lv_layer_t layer;
    lv_canvas_init_layer(s_bg_canvas, &layer);
    fill_screen(&layer, PAPER, LV_OPA_COVER);
    s_part = PART_BG;
    scene_draw(&layer);
    s_part = PART_ALL;
    lv_canvas_finish_layer(s_bg_canvas, &layer);
    LV_PROFILER_END_TAG("pp_bg_render");
}

/** The menu's icons for the way the middle faces now, each into its image (under the lock). */
static void icons_render(void) {
    LV_PROFILER_BEGIN_TAG("pp_icons_render");
    const bool veil = s_veil;
    s_veil = false;  // a cached image is drawn plain; pasting it under Yay's veil recolours it
    for (int i = 0; i < 5; i++)
        for (int k = 0; k < ICON_SIZES; k++) {
            if (!s_icon[i][k]) continue;
            lv_draw_buf_clear(s_icon[i][k], NULL);
            lv_canvas_set_draw_buf(s_bg_canvas, s_icon[i][k]);
            lv_layer_t layer;
            lv_canvas_init_layer(s_bg_canvas, &layer);
            game_icon(&layer, i, ICON_PX / 2.0f, ICON_PX / 2.0f, 19 + 4.0f * k / (ICON_SIZES - 1), P->rot, LV_OPA_COVER);
            lv_canvas_finish_layer(s_bg_canvas, &layer);
        }
    lv_canvas_set_draw_buf(s_bg_canvas, s_bg);
    s_icon_rot = P->rot;
    s_veil = veil;
    LV_PROFILER_END_TAG("pp_icons_render");
}

/** Ten seconds' digits for the way the middle faces now, into their masks a few at a time (under the lock): about
 *  6 ms a frame, so entering the game or a turn does not stall; a digit without its mask yet is drawn as a letter. */
static void glyphs_step(void) {
    static int done;  // masks made for this facing, in ORDER
    static const uint8_t ORDER[11] = {1, 0, 10, 2, 3, 4, 5, 6, 7, 8, 9};  // "10.00" first
    if (fabsf(s_glyph_rot - P->rot) > 1e-4f) {
        for (int i = 0; i < 11; i++) lv_draw_buf_destroy(s_glyph[i]), s_glyph[i] = NULL;
        done = 0;
        s_glyph_rot = P->rot;
    }
    if (done >= 11) return;
    const lv_font_t *font = cjk_font_get(GLYPH_SIZE);
    lv_draw_buf_t *tmp = font ? lv_draw_buf_create(GLYPH_PX, GLYPH_PX, LV_COLOR_FORMAT_A8, LV_STRIDE_AUTO) : NULL;
    if (!tmp) return;
    LV_PROFILER_BEGIN_TAG("pp_glyphs_step");
    int32_t deg10 = ri(P->rot * 1800 / PP_PI) % 3600;
    if (deg10 < 0) deg10 += 3600;
    lv_draw_letter_dsc_t d;
    lv_draw_letter_dsc_init(&d);
    d.font = font;
    d.rotation = deg10;
    const int64_t until = esp_timer_get_time() + 6000;
    while (done < 11 && esp_timer_get_time() < until) {
        const int i = ORDER[done++];
        d.unicode = i < 10 ? '0' + i : '.';
        lv_draw_buf_clear(tmp, NULL);
        lv_canvas_set_draw_buf(s_bg_canvas, tmp);
        lv_layer_t layer;
        lv_canvas_init_layer(s_bg_canvas, &layer);
        const lv_point_t q = {GLYPH_PX / 2, GLYPH_PX / 2};  // the point lv_draw_letter turns it about: the middle
        lv_draw_letter(&layer, &d, &q);
        lv_canvas_finish_layer(s_bg_canvas, &layer);
        // kept cropped to where there is any coverage
        const uint32_t st = tmp->header.stride;
        int x0 = GLYPH_PX, y0 = GLYPH_PX, x1 = -1, y1 = -1;
        for (int y = 0; y < GLYPH_PX; y++)
            for (int x = 0; x < GLYPH_PX; x++)
                if (tmp->data[y * st + x]) x0 = LV_MIN(x0, x), y0 = LV_MIN(y0, y), x1 = LV_MAX(x1, x), y1 = LV_MAX(y1, y);
        lv_draw_buf_t *b = x1 >= 0 ? lv_draw_buf_create(x1 - x0 + 1, y1 - y0 + 1, LV_COLOR_FORMAT_A8, LV_STRIDE_AUTO) : NULL;
        if (!b) continue;
        for (int y = y0; y <= y1; y++) memcpy(b->data + (y - y0) * b->header.stride, tmp->data + y * st + x0, x1 - x0 + 1);
        s_glyph[i] = b;
        s_glyph_off[i] = (lv_point_t){x0 - GLYPH_PX / 2, y0 - GLYPH_PX / 2};
    }
    lv_canvas_set_draw_buf(s_bg_canvas, s_bg);
    lv_draw_buf_destroy(tmp);
    LV_PROFILER_END_TAG("pp_glyphs_step");
}

/** One game's menu middle into its buffer (under the lock): the buffer stands for the middle of the screen. */
static void center_render(int i) {
    LV_PROFILER_BEGIN_TAG("pp_center_render");
    const bool veil = s_veil;
    s_veil = false;
    lv_draw_buf_clear(s_center[i], NULL);
    lv_canvas_set_draw_buf(s_bg_canvas, s_center[i]);
    lv_layer_t layer;
    lv_canvas_init_layer(s_bg_canvas, &layer);
    lv_area_move(&layer.buf_area, PP_C - CENTER_PX / 2, PP_C - CENTER_PX / 2);  // drawn in screen coordinates
    layer._clip_area = layer.phy_clip_area = layer.buf_area;
    menu_center(&layer, i);
    lv_canvas_finish_layer(s_bg_canvas, &layer);
    lv_canvas_set_draw_buf(s_bg_canvas, s_bg);
    s_center_key[i] = center_key();
    s_veil = veil;
    LV_PROFILER_END_TAG("pp_center_render");
}

/** While nothing on the menu moves: the next middle not drawn for the way it faces now (the chosen one, then
 *  outwards), one a frame. */
static void center_ahead(void) {
    const uint32_t k = center_key();
    const int sel = menu_sel();
    for (int j = 0; j < 5; j++) {
        const int i = (sel + (j + 1) / 2 * (j & 1 ? 1 : -1) + 5) % 5;
        if (s_center[i] && s_center_key[i] != k) {
            center_render(i);
            return;
        }
    }
}

static void stage_draw(lv_event_t *e) {
    lv_layer_t *L = lv_event_get_layer(e);
    LV_PROFILER_BEGIN_TAG("pp_stage_draw");  // making the draw tasks, once a band (the drawing itself is LVGL's)
    if (s_bg_key) {
        lv_draw_image_dsc_t d;
        lv_draw_image_dsc_init(&d);
        d.src = s_bg;
        const lv_area_t a = {0, 0, PP_W - 1, PP_W - 1};
        lv_draw_image(L, &d, &a);
        s_part = PART_FG;
    }
    scene_draw(L);
    s_part = PART_ALL;
    draw_parts(L);
    if (P->battery == 1 && P->now - P->bat_flash < 1.2f)
        arc(L, PP_C, PP_C, 229, 6, 0, PP_TAU, AMBER, (lv_opa_t)(sinf(pp_clampf((P->now - P->bat_flash) / 1.2f, 0, 1) * PP_PI) * 255), false);
    if (P->sheet) draw_sheet(L);
    LV_PROFILER_END_TAG("pp_stage_draw");
}

// ───────────────────────── what to redraw
typedef struct {
    lv_area_t a[64];
    int n;
} rects_t;
#define INV_MAX 30  // LVGL keeps 32 areas a frame and redraws the whole screen past that: this frame's and the last's
static rects_t s_prev, s_cur;
static uint32_t s_sig = 0;
static float s_last_rot = 1e9f, s_last_menu = 1e9f;
static int s_last_sel = -1;

static void add_rect(float x0, float y0, float x1, float y1) {
    if (s_cur.n >= (int)(sizeof(s_cur.a) / sizeof(s_cur.a[0]))) return;
    lv_area_t a = {ri(fmaxf(0, x0)), ri(fmaxf(0, y0)), ri(fminf(PP_W - 1, x1)), ri(fminf(PP_W - 1, y1))};
    if (a.x2 < a.x1 || a.y2 < a.y1) return;
    s_cur.a[s_cur.n++] = a;
}
static void add_box(float x, float y, float r) { add_rect(x - r, y - r, x + r, y + r); }
static void add_sector(float r0, float r1, float lo, float hi);
/** The ring at the edge (radius 223-229: the reaction game's end, ten seconds, the bomb's urging, low battery) as 12
 *  boxes along it, not the four edges of the screen. */
static void add_rim(void) {
    for (int k = 0; k < 12; k++) add_sector(220, 233, k * PP_TAU / 12, (k + 1) * PP_TAU / 12);
}
/** A box in the frame of whoever the middle faces (centre fx, fy; half sizes hw, hh), turned onto the screen. */
static void add_frame_box(float fx, float fy, float hw, float hh) {
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (int k = 0; k < 4; k++) {
        float x, y;
        to_screen(P->rot, fx + (k & 1 ? hw : -hw), fy + (k & 2 ? hh : -hh), &x, &y);
        x0 = fminf(x0, x), y0 = fminf(y0, y), x1 = fmaxf(x1, x), y1 = fmaxf(y1, y);
    }
    add_rect(x0, y0, x1, y1);
}
static void add_sector(float r0, float r1, float lo, float hi) {
    float b[4];
    sector_box(PP_C, PP_C, r0, r1, lo, hi, b);
    add_rect(b[0] - 4, b[1] - 4, b[2] + 4, b[3] + 4);
}

static uint32_t fnv(uint32_t h, int v) { return (h ^ (uint32_t)v) * 16777619u; }

static uint32_t signature(void) {
    uint32_t h = 2166136261u;
    h = fnv(h, P->scene);
    h = fnv(h, (int)lroundf(P->rot_target * 1000));
    h = fnv(h, P->sheet);
    h = fnv(h, P->en);
    h = fnv(h, P->battery);
    h = fnv(h, P->battery_pct);
    h = fnv(h, s_veil);
    switch (P->scene) {
    case PP_SC_SEATS: h = fnv(h, P->seating.n), h = fnv(h, P->color_at); break;
    case PP_SC_DICE: h = fnv(h, P->dice_st), h = fnv(h, P->dice.n), h = fnv(h, P->dice_special); break;
    case PP_SC_WHEEL: h = fnv(h, P->wheel_st), h = fnv(h, P->seats.n), h = fnv(h, P->spins > 0); break;
    case PP_SC_REACT: {
        const pp_react_t *r = &P->react;
        h = fnv(h, r->st), h = fnv(h, r->winner), h = fnv(h, P->seats.n);
        for (int i = 0; i < r->n; i++) h = fnv(h, r->score[i] * 2 + r->locked[i]);
        break;
    }
    case PP_SC_BOMB: h = fnv(h, P->bomb.st), h = fnv(h, P->bomb.penalty), h = fnv(h, P->bomb.urging); break;
    case PP_SC_TEN: {
        const pp_ten_t *t = &P->ten;
        h = fnv(h, t->st), h = fnv(h, t->mode), h = fnv(h, t->turn), h = fnv(h, (int)lroundf(t->best * 100));
        for (int i = 0; i < t->n; i++) h = fnv(h, t->has[i]);
        break;
    }
    case PP_SC_SETTINGS:
        h = fnv(h, P->set_sel), h = fnv(h, P->players), h = fnv(h, P->dice_n), h = fnv(h, P->volume), h = fnv(h, P->penalties);
        break;
    case PP_SC_SCORES:
        for (int i = 0; i < P->seats.n; i++) h = fnv(h, P->wins[i]);
        break;
    default: break;
    }
    return h;
}

static bool dynamic_rects(void) {
    // returns true when the whole screen must be redrawn this frame
    const bool beating = P->now - P->result_at < 3.0f;  // pp_play_pulse(): then it holds still
    switch (P->scene) {
    case PP_SC_MENU:
        // the icons turning: their boxes (where they were comes from the last frame's rects); a new selection: the
        // middle (its icon, name and hint), never the whole screen
        if (fabsf(P->menu_pos - s_last_menu) > 1e-4f) {
            for (int i = 0; i < 5; i++) {
                float x, y;
                icon_pos(i, &x, &y);
                add_box(x, y, 44);
            }
        }
        if (menu_sel() != s_last_sel) add_box(PP_C, PP_C, 128);
        s_last_menu = P->menu_pos;
        s_last_sel = menu_sel();
        break;
    case PP_SC_SEATS:
        for (int i = 0; i < P->seating.n; i++)
            if (P->now - P->seat_born[i] < 0.35f) add_box(PP_C + 206 * cosf(P->seating.s[i].a), PP_C + 206 * sinf(P->seating.s[i].a), 18);
        break;
    case PP_SC_DICE:
        for (int i = 0; i < P->dice.n; i++) {
            const pp_die_t *d = &P->dice.d[i];
            if (P->dice_st == PP_DICE_ROLL || P->dice_st == PP_DICE_POP || P->dice_st == PP_DICE_ARRANGE || d->tum > 0)
                add_box(PP_C + d->x, PP_C + d->y, PP_DIE_PX * P->scale[i] * 0.8f + 8);
        }
        break;
    case PP_SC_WHEEL: {
        static float last_th = 1e9f;
        if (fabsf(P->wheel.th - last_th) > 1e-4f) {
            for (int k = 0; k < 2; k++) {  // the needle, as four small boxes along it, where it was and is
                const float th = k ? P->wheel.th : last_th;
                if (th > 1e8f) continue;
                for (int j = -1; j < 5; j++) {
                    const float d = j * 36.0f;
                    add_box(PP_C + cosf(th) * d, PP_C + sinf(th) * d, 26);
                }
            }
            last_th = P->wheel.th;
        }
        for (int i = 0; i < P->seats.n; i++) {
            const bool win = P->wheel_st == PP_WHEEL_RESULT && i == P->wheel.winner;
            if (P->flash[i] > 0 || (win && beating)) {
                float lo, hi;
                pp_seats_sector(&P->seats, i, &lo, &hi);
                add_sector(150, 222, lo, hi);
            }
        }
        break;
    }
    case PP_SC_REACT: {
        const pp_react_t *r = &P->react;
        if (r->st == PP_RX_WAIT) add_box(PP_C, PP_C, 60);
        if (r->st == PP_RX_OVER) {
            if (r->t < 2.3f) add_rim();  // the sage arc round the edge: drawn in, then faded by 2.2 s
            if (beating) {
                float lo, hi;
                pp_seats_sector(&P->seats, r->winner, &lo, &hi);
                add_sector(100, 228, lo, hi);
            }
        }
        break;
    }
    case PP_SC_BOMB: {
        const pp_bomb_t *b = &P->bomb;
        if (b->st == PP_BOMB_BOOM && b->t < 0.95f) return true;
        if (b->st == PP_BOMB_LIT) {
            float x, y;
            fuse_tip(&x, &y);
            add_box(x, y, 40);
            float c0x, c0y;
            to_screen(P->rot, sinf(0.6f) * 82 * 1.05f, 14 - cosf(0.6f) * 82 * 1.05f, &c0x, &c0y);
            add_rect(fminf(x, c0x) - 10, fminf(y, c0y) - 10, fmaxf(x, c0x) + 10, fmaxf(y, c0y) + 10);
            if (b->urging) {
                add_rim();
                float cx, cy;
                to_screen(P->rot, 0, 14, &cx, &cy);
                add_box(cx, cy, 92);
            }
        }
        if (b->st == PP_BOMB_IDLE && b->t < 0.4f) add_box(PP_C, PP_C, 120);
        if (b->st == PP_BOMB_PENALTY && b->t < 0.3f) add_box(PP_C, PP_C, 150);
        break;
    }
    case PP_SC_TEN: {
        const pp_ten_t *t = &P->ten;
        if (t->st == PP_TEN_RUN) {
            // the number while it shows (and the frame it goes), then only the pulsing dot; the ring all round while it
            // fades, otherwise only where its end moved
            static float last_v = -1, last_a = 1e9f;
            const float v = ten_shown(t), a = -PP_PI / 2 + PP_TAU * pp_clampf(t->t / 10, 0, 1);
            if (v > 0.01f || last_v > 0.01f) {
                add_frame_box(0, -10, 135, 55);
            } else {
                float x, y;
                to_screen(P->rot, 0, -10, &x, &y);
                add_box(x, y, 10);
            }
            if (v != last_v) add_rim();
            else if (v > 0.01f) add_sector(220, 233, fminf(a, last_a), fmaxf(a, last_a));
            last_v = v, last_a = a;
        }
        if (t->st == PP_TEN_SHOWN && t->shown_t < 0.6f) add_rim();
        if (t->st == PP_TEN_FINAL && beating) {
            float lo, hi;
            if (t->winner >= 0) {
                pp_seats_sector(&P->seats, t->winner, &lo, &hi);
                add_sector(160, 222, lo, hi);
            }
        }
        break;
    }
    default: break;
    }
    if (P->battery == 1 && P->now - P->bat_flash < 1.3f) add_rim();
    // particles: the 78 px cells they touch, each row's runs of cells as one box and the same runs in the next rows
    // joined (one box round them all would redraw most of the screen while confetti flies out)
    if (P->nparts) {
        enum { CELL = 78, CELLS = (PP_W + CELL - 1) / CELL };
        uint8_t row[CELLS] = {0};
        for (int i = 0; i < P->nparts; i++) {
            const pp_part_t *q = &P->parts[i];
            if (behind_yay(q)) continue;
            const float r = q->kind == PP_PART_SMOKE ? 80 : SPR_PX / 2 + 1;
            const int cx0 = pp_clampi((int)((q->x - r) / CELL), 0, CELLS - 1), cx1 = pp_clampi((int)((q->x + r) / CELL), 0, CELLS - 1);
            const int cy0 = pp_clampi((int)((q->y - r) / CELL), 0, CELLS - 1), cy1 = pp_clampi((int)((q->y + r) / CELL), 0, CELLS - 1);
            for (int y = cy0; y <= cy1; y++)
                for (int x = cx0; x <= cx1; x++) row[y] |= (uint8_t)(1u << x);
        }
        for (int y = 0; y < CELLS; y++) {
            int y1 = y;
            while (y1 + 1 < CELLS && row[y1 + 1] == row[y]) y1++;
            for (int x = 0; x < CELLS; x++) {
                if (!(row[y] >> x & 1)) continue;
                int x1 = x;
                while (x1 + 1 < CELLS && (row[y] >> (x1 + 1) & 1)) x1++;
                add_rect(x * CELL, y * CELL, (x1 + 1) * CELL - 1, (y1 + 1) * CELL - 1);
                x = x1;
            }
            y = y1;
        }
    }
    return false;
}

/** Hands LVGL this frame's boxes and the last frame's (what was drawn there goes), each once. Past what LVGL keeps
 *  they become the runs of a coarse grid's cells (39 px, else 78, else 156), joined down where the rows match.
 *  Returns the pixels. */
static uint32_t invalidate_dirty(void) {
    static lv_area_t all[2 * sizeof(s_cur.a) / sizeof(s_cur.a[0])];
    int n = 0;
    for (int i = 0; i < s_cur.n; i++) all[n++] = s_cur.a[i];
    for (int i = 0; i < s_prev.n; i++) {
        bool in = false;
        for (int j = 0; j < s_cur.n && !in; j++) {
            const lv_area_t *p = &s_prev.a[i], *c = &s_cur.a[j];
            in = p->x1 >= c->x1 && p->y1 >= c->y1 && p->x2 <= c->x2 && p->y2 <= c->y2;
        }
        if (!in) all[n++] = s_prev.a[i];
    }
    uint32_t px = 0;
    if (n <= INV_MAX) {
        for (int i = 0; i < n; i++) lv_obj_invalidate_area(s_stage, &all[i]), px += lv_area_get_size(&all[i]);
        return px;
    }
    for (int cell = 39;; cell *= 2) {
        const int cells = (PP_W + cell - 1) / cell;  // 12, 6, 3
        uint16_t row[12] = {0};
        for (int i = 0; i < n; i++)
            for (int y = all[i].y1 / cell; y <= all[i].y2 / cell; y++)
                for (int x = all[i].x1 / cell; x <= all[i].x2 / cell; x++) row[y] |= (uint16_t)(1u << x);
        lv_area_t out[INV_MAX];
        int m = 0;
        bool fits = true;
        for (int y = 0; y < cells && fits; y++) {
            int y1 = y;
            while (y1 + 1 < cells && row[y1 + 1] == row[y]) y1++;
            for (int x = 0; x < cells; x++) {
                if (!(row[y] >> x & 1)) continue;
                int x1 = x;
                while (x1 + 1 < cells && (row[y] >> (x1 + 1) & 1)) x1++;
                if (m == INV_MAX) {
                    fits = false;
                    break;
                }
                out[m++] = (lv_area_t){x * cell, y * cell, LV_MIN((x1 + 1) * cell, PP_W) - 1, LV_MIN((y1 + 1) * cell, PP_W) - 1};
                x = x1;
            }
            y = y1;
        }
        if (fits || cell >= 156) {  // 3 x 3 cells always fit
            for (int i = 0; i < m; i++) lv_obj_invalidate_area(s_stage, &out[i]), px += lv_area_get_size(&out[i]);
            return px;
        }
    }
}

void pp_ui_invalidate_all(void) {
    if (s_stage) lv_obj_invalidate(s_stage);
    s_sig = 0;
}

void pp_ui_frame(void) {
    // screen off or face down: nothing is drawn (it would go to a dark panel as fast as it can, and keep CPU 0 busy
    // for nothing); Yay put away; the whole screen once on waking
    static bool dark;
    if (P->off || P->face_down) {
        if (!dark && s_yay) {
            lv_obj_add_flag(yk_yay_obj(s_yay), LV_OBJ_FLAG_HIDDEN);
            s_yay_serial = UINT32_MAX;  // shown again on waking, as the model has it then
        }
        dark = true;
        return;
    }
    if (dark) {
        dark = false;
        pp_ui_invalidate_all();
    }
    yay_update();
    s_veil = s_yay && P->yay.mode == PP_YAY_HOST;
    // the cached parts this frame needs, drawn before it: their time counts in the frame's
    const int64_t t0 = esp_timer_get_time();
    if (P->scene == PP_SC_MENU && s_bg_canvas && fabsf(s_icon_rot - P->rot) > 1e-4f) icons_render();
    if (P->scene == PP_SC_TEN && s_bg_canvas) glyphs_step();
    // the still part changed: this frame draws everything (cheaper than buffering it first), the buffer waits
    const uint32_t key = s_bg ? bg_key() : 0;
    if (key != s_bg_want) {
        s_bg_want = key;
        s_bg_key = 0;
        s_sig = 0;  // and the whole screen
    }
    if (P->scene == PP_SC_MENU && s_bg_canvas) {  // the chosen game's middle, into its buffer once, then pasted
        const int sel = menu_sel();
        if (s_center[sel] && s_center_key[sel] != center_key()) center_render(sel);
    }
    s_bg_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    if (s_bg_ms > s_st.bg_ms_max) s_st.bg_ms_max = s_bg_ms;
    const uint32_t sig = signature();
    s_cur.n = 0;
    // a new scene or a turn: the whole screen once (while the panel is dark, pp_play_light), not each frame of it
    bool full = sig != s_sig || fabsf(P->rot - s_last_rot) > 1e-4f;
    if (!full) full = dynamic_rects();
    else dynamic_rects();  // keep its "last" positions current
    s_last_rot = P->rot;
    s_sig = sig;
    uint32_t px = 0;
    // the still part into its buffer once that pays: a frame with nothing to redraw (unseen), or a redraw big enough
    // to draw it all again (confetti, the needle sweeping); never on the frame it changed, which is drawn whole
    if (s_bg_want && !s_bg_key && !full) {
        uint32_t area = 0;
        for (int i = 0; i < s_cur.n; i++) area += lv_area_get_size(&s_cur.a[i]);
        for (int i = 0; i < s_prev.n; i++) area += lv_area_get_size(&s_prev.a[i]);
        if (area == 0 || area >= BG_WORTH_PX) {
            const int64_t t1 = esp_timer_get_time();
            bg_render();
            s_bg_key = s_bg_want;
            const uint32_t ms = (uint32_t)((esp_timer_get_time() - t1) / 1000);
            if (area) s_bg_ms += ms;  // in this frame's time (with nothing to redraw there is no frame)
            if (ms > s_st.bg_ms_max) s_st.bg_ms_max = ms;
        }
    }
    if (P->scene == PP_SC_MENU && !full && !s_prev.n && !s_cur.n && s_bg_canvas) center_ahead();
    if (full) {
        lv_obj_invalidate(s_stage);
        s_st.full++;
        px = PP_W * PP_W;
    } else {
        px = invalidate_dirty();
    }
    s_prev = s_cur;
    s_px_total += px;
    s_full_px = px;
}

// ───────────────────────── touch, timing, setup
static void stage_cover(lv_event_t *e) {
    if (s_bg_key) lv_event_set_cover_res(e, LV_COVER_RES_COVER);
}

static void stage_touch(lv_event_t *e) {
    const lv_event_code_t code = lv_event_get_code(e);
    lv_point_t pt;
    lv_indev_get_point(lv_indev_active(), &pt);
    if (!s_touch) return;
    if (code == LV_EVENT_PRESSED) s_touch(0, pt.x, pt.y);
    else if (code == LV_EVENT_PRESSING) s_touch(1, pt.x, pt.y);
    else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) s_touch(2, pt.x, pt.y);
}

static void on_refr(lv_event_t *e) {
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_REFR_START) {
        s_refr_start = esp_timer_get_time();
    } else if (code == LV_EVENT_REFR_READY && s_refr_start) {
        // the frame: what it cached before it, then drawing and sending it
        const uint32_t ms = (uint32_t)((esp_timer_get_time() - s_refr_start) / 1000) + s_bg_ms;
        s_st.frames++;
        s_render_total += ms;
        if (ms > s_st.render_ms_max) s_st.render_ms_max = ms;
        s_refr_start = 0;
        if (ms > 45) {  // which frames are slow, and what was on screen
            const int sub = P->scene == PP_SC_REACT ? P->react.st : P->scene == PP_SC_WHEEL ? P->wheel_st
                          : P->scene == PP_SC_DICE  ? P->dice_st  : P->scene == PP_SC_BOMB  ? P->bomb.st
                          : P->scene == PP_SC_TEN   ? P->ten.st   : 0;
            printf("PERF_SLOW ms=%u cache_ms=%u scene=%s st=%d px=%u parts=%d yay=%d sheet=%d\n", (unsigned)ms, (unsigned)s_bg_ms,
                   pp_scene_name(P->scene), sub, (unsigned)s_full_px, P->nparts, P->yay.mode, P->sheet);
        }
        s_bg_ms = 0;
    }
}

void pp_ui_stats(pp_ui_stats_t *out, bool reset) {
    *out = s_st;
    out->render_ms_avg = s_st.frames ? (uint32_t)(s_render_total / s_st.frames) : 0;
    out->px_avg = s_st.frames ? (uint32_t)(s_px_total / s_st.frames) : 0;
    if (reset) {
        memset(&s_st, 0, sizeof(s_st));
        s_render_total = s_px_total = 0;
    }
}

void pp_ui_init(pp_play_t *play, pp_ui_touch_fn on_touch) {
    P = play;
    s_touch = on_touch;
    make_faces();
    make_sprites();
    // every size the screens use, made now rather than in the middle of drawing (cjk_font keeps up to 12)
    static const int SIZES[] = {18, 24, 34, 56, 88};
    for (size_t i = 0; i < sizeof(SIZES) / sizeof(SIZES[0]); i++)
        if (!cjk_font_get(SIZES[i])) ESP_LOGW(TAG, "no font at %d px", SIZES[i]);
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_scrollable(scr, false);
    lv_obj_set_style_bg_color(scr, col(PAPER), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    s_stage = lv_obj_create(scr);
    lv_obj_remove_style_all(s_stage);
    lv_obj_set_size(s_stage, PP_W, PP_W);
    lv_obj_set_pos(s_stage, 0, 0);
    lv_obj_add_flag(s_stage, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_stage, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_stage, stage_draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(s_stage, stage_cover, LV_EVENT_COVER_CHECK, NULL);
    lv_obj_add_event_cb(s_stage, stage_touch, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_stage, stage_touch, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_stage, stage_touch, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_stage, stage_touch, LV_EVENT_PRESS_LOST, NULL);

    // Transparent around its outline: hosting it stands on the dimmed game, peeking it overlaps slices and dice
    s_yay = yk_yay_create(scr, 176, PP_C, PP_C, YK_YAY_TRANSPARENT);
    if (s_yay) {
        lv_obj_remove_flag(yk_yay_obj(s_yay), LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(yk_yay_obj(s_yay), LV_OBJ_FLAG_HIDDEN);
    } else {
        ESP_LOGE(TAG, "no memory for Yay");
    }

    // the background buffer (PSRAM, 434 KB) and a hidden canvas to draw into it; without memory, no cache
    s_bg = lv_draw_buf_create(PP_W, PP_W, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
    if (s_bg) {
        s_bg_canvas = lv_canvas_create(scr);
        lv_canvas_set_draw_buf(s_bg_canvas, s_bg);
        lv_obj_add_flag(s_bg_canvas, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < 5; i++) {
            for (int k = 0; k < ICON_SIZES; k++) s_icon[i][k] = lv_draw_buf_create(ICON_PX, ICON_PX, LV_COLOR_FORMAT_ARGB8888, LV_STRIDE_AUTO);
            s_center[i] = lv_draw_buf_create(CENTER_PX, CENTER_PX, LV_COLOR_FORMAT_ARGB8888, LV_STRIDE_AUTO);
        }
    } else {
        ESP_LOGW(TAG, "no memory for the background: every frame drawn whole");
    }

    lv_display_t *disp = lv_obj_get_display(scr);
    lv_display_add_event_cb(disp, on_refr, LV_EVENT_REFR_START, NULL);
    lv_display_add_event_cb(disp, on_refr, LV_EVENT_REFR_READY, NULL);
    lv_screen_load(scr);
}
