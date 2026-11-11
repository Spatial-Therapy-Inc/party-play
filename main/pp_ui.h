// Party Play's screen (docs/product/party-play-ui.md): draws pp_play_t with LVGL. One custom-drawn stage holds the
// graphics and the turned text (each glyph placed and rotated towards whoever the middle faces); Yay is an image
// object over it. Only what changed is redrawn (docs/product/party-play.md "帧率").
#pragma once

#include <stdint.h>

#include "lvgl.h"
#include "pp_play.h"

typedef void (*pp_ui_touch_fn)(int phase, float x, float y);  // phase: 0 down, 1 move, 2 up

/** Builds the screen; call with the board lock held, after cjk_font_init(). */
void pp_ui_init(pp_play_t *play, pp_ui_touch_fn on_touch);
/** After each pp_play_tick: marks what changed for redraw and moves Yay. With the board lock held. */
void pp_ui_frame(void);
/** Redraws everything on the next refresh. */
void pp_ui_invalidate_all(void);

typedef struct {
    uint32_t frames, render_ms_avg, render_ms_max, full;  // a frame's time: its caches drawn first, then the frame
    uint32_t px_avg;     // invalidated pixels per frame
    uint32_t bg_ms_max;  // the longest of those cache drawings (a scene's still part, the menu's icons and middle)
} pp_ui_stats_t;
void pp_ui_stats(pp_ui_stats_t *out, bool reset);
