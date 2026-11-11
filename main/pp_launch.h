// Launch parameters from Yay (yk_launch params, docs/product/party-play.md P3): {"game":"wheel","players":5,"dice":3}.
// Unknown fields are ignored, an unknown game opens the menu, numbers are clamped into range. Plain C, host tested.
#pragma once

#include <stdbool.h>

typedef struct {
    int game;     // pp_game_t, or -1 for the menu
    int players;  // 2-8, or 0 when not given
    int dice;     // 1-3, or 0 when not given
} pp_launch_params_t;

/** Never fails: anything it cannot read is left out. json may be NULL. */
void pp_launch_parse(const char *json, pp_launch_params_t *out);
