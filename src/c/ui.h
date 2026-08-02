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

/**
 * Shared look. Kept in one place so the redesign has a single dial to turn.
 *
 * Taken from the Alloy version's theme.js: black ground, plain blue as the
 * selection fill, and white type throughout -- including on the selection,
 * where styles.itemSelected also kept `color: "white"`.
 */
#define UI_COLOR_BACKGROUND     GColorBlack
#define UI_COLOR_FOREGROUND     GColorWhite
#define UI_COLOR_HIGHLIGHT      GColorBlue
#define UI_COLOR_HIGHLIGHT_TEXT GColorWhite

/** Structural labels only -- section headers, not content. */
#define UI_COLOR_MUTED          GColorLightGray

void ui_style_menu_layer(MenuLayer *menu_layer);

/**
 * List rendering, shared by both menu screens.
 *
 * The SDK's menu_cell_basic_draw uses an 18 px title, much smaller than the
 * Alloy version's "28px Gothic". These draw at the old size instead; see ui.c.
 */
void ui_draw_menu_row(GContext *ctx, const Layer *cell_layer, const char *text);
void ui_draw_menu_header(GContext *ctx, const Layer *cell_layer,
                         const char *text);
int16_t ui_menu_cell_height(bool highlighted);
int16_t ui_menu_header_height(void);

/**
 * Marquee for the selected row, so a name wider than the screen can be read.
 *
 * Only one row scrolls, only on the list that currently has the screen, and
 * only while its text actually overhangs -- ui_draw_menu_row() decides that as
 * it draws, and the timer stops on its own when nothing overflows.
 *
 * A list window hands over its MenuLayer on appear and clears it on disappear;
 * both are required, or the timer would keep redrawing a hidden layer.
 */
void ui_marquee_set_menu(MenuLayer *menu_layer);

/** Puts the marquee back to the start. Call whenever the selection moves. */
void ui_marquee_reset(void);

/**
 * A single line of text that scrolls sideways when it does not fit.
 *
 * Track and artist names routinely run past 200 px at a readable size, and a
 * trailing ellipsis hides exactly the part that tells two of them apart. Unlike
 * the menu marquee above this is a Layer of its own, so it can be used outside
 * a MenuLayer; several may scroll at once and they share one timer.
 *
 * The text is NOT copied -- the pointer must stay valid, as with TextLayer.
 */
typedef struct MarqueeLabel MarqueeLabel;

MarqueeLabel *marquee_label_create(GRect frame, const char *font_key,
                                   GColor color, GTextAlignment alignment);
void marquee_label_destroy(MarqueeLabel *label);
Layer *marquee_label_get_layer(MarqueeLabel *label);

/** Sets the text and returns the line to the start of its travel. */
void marquee_label_set_text(MarqueeLabel *label, const char *text);
