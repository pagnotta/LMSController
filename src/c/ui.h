#pragma once

#include <pebble.h>
#include "lms.h"

/**
 * The three screens, in the order they stack.
 *
 * Navigation is the window stack itself rather than a state machine: Back pops,
 * which is what the hardware button does anyway. The browse screen pushes one
 * window per menu level, so stepping out of a folder needs no bookkeeping.
 */

void players_window_push(void);
void status_window_push(const char *player_id, const char *player_name);

/** Opens the LMS menu at the top level for this player. */
void browse_window_push_root(const char *player_id);

/** Closes every open browse level, leaving the status screen on top. */
void browse_close_all(void);

/** Shared look. Kept in one place so the redesign has a single dial to turn. */
#define UI_COLOR_BACKGROUND     GColorBlack
#define UI_COLOR_FOREGROUND     GColorWhite
#define UI_COLOR_HIGHLIGHT      GColorVividCerulean
#define UI_COLOR_HIGHLIGHT_TEXT GColorBlack
#define UI_COLOR_MUTED          GColorLightGray

void ui_style_menu_layer(MenuLayer *menu_layer);
