#include "ui.h"

/**
 * Shared list rendering.
 *
 * The default `menu_cell_basic_draw` puts an 18 px title in a cell sized for
 * it, which is markedly smaller than the Alloy version this replaces -- that
 * used "28px Gothic" at a row height of 42 (48 on round), and it was readable
 * at arm's length. These helpers reproduce those numbers, so both list screens
 * stay identical and the redesign has one place to change.
 */

// Matches metrics.rowHeight from the Alloy theme.js.
#define ROW_HEIGHT_RECT 42

// A round display shows a tall focused row and shorter neighbours. The SDK's
// MENU_CELL_ROUND_UNFOCUSED_SHORT_CELL_HEIGHT of 24 cannot hold 28 px text, so
// the unfocused rows are taller than the default here.
#define ROW_HEIGHT_ROUND_FOCUSED   68
#define ROW_HEIGHT_ROUND_UNFOCUSED 40

#define HEADER_HEIGHT 26

// Marquee pacing. 40 ms is smooth enough on a Memory LCD without redrawing
// more often than the screen is worth, and the holds give the eye time to read
// each end before it moves.
#define MARQUEE_TICK_MS 40
#define MARQUEE_STEP_PX 2
#define MARQUEE_HOLD_START_TICKS 30
#define MARQUEE_HOLD_END_TICKS 25

static struct {
  MenuLayer *menu;
  AppTimer *timer;
  int16_t offset;    //!< pixels the text is currently shifted left
  int16_t overflow;  //!< how far it extends past the cell; 0 means no scrolling
  int16_t hold;      //!< ticks left to sit still at one end
} s_marquee;

void ui_style_menu_layer(MenuLayer *menu_layer) {
  menu_layer_set_normal_colors(menu_layer, UI_COLOR_BACKGROUND,
                               UI_COLOR_FOREGROUND);
  menu_layer_set_highlight_colors(menu_layer, UI_COLOR_HIGHLIGHT,
                                  UI_COLOR_HIGHLIGHT_TEXT);
}

int16_t ui_menu_cell_height(bool highlighted) {
  return PBL_IF_ROUND_ELSE(
      highlighted ? ROW_HEIGHT_ROUND_FOCUSED : ROW_HEIGHT_ROUND_UNFOCUSED,
      ROW_HEIGHT_RECT);
}

int16_t ui_menu_header_height(void) {
  return HEADER_HEIGHT;
}

static void prv_marquee_tick(void *ctx);

static void prv_marquee_run(void) {
  if (!s_marquee.timer && s_marquee.menu)
    s_marquee.timer = app_timer_register(MARQUEE_TICK_MS, prv_marquee_tick,
                                         NULL);
}

/**
 * Advances the marquee and asks the list to redraw.
 *
 * `overflow` is whatever the last draw of the selected row reported. It is
 * cleared there too, so a selection whose text fits stops the timer on the
 * next tick rather than spinning on for the life of the window.
 */
static void prv_marquee_tick(void *ctx) {
  s_marquee.timer = NULL;

  if (!s_marquee.menu || s_marquee.overflow <= 0) {
    s_marquee.offset = 0;
    return;
  }

  if (s_marquee.hold > 0) {
    s_marquee.hold--;
  } else if (s_marquee.offset < s_marquee.overflow) {
    s_marquee.offset += MARQUEE_STEP_PX;
    if (s_marquee.offset >= s_marquee.overflow) {
      s_marquee.offset = s_marquee.overflow;
      s_marquee.hold = MARQUEE_HOLD_END_TICKS;
    }
  } else {
    s_marquee.offset = 0;
    s_marquee.hold = MARQUEE_HOLD_START_TICKS;
  }

  layer_mark_dirty(menu_layer_get_layer(s_marquee.menu));
  prv_marquee_run();
}

void ui_marquee_set_menu(MenuLayer *menu_layer) {
  s_marquee.menu = menu_layer;
  ui_marquee_reset();
  if (!menu_layer && s_marquee.timer) {
    app_timer_cancel(s_marquee.timer);
    s_marquee.timer = NULL;
  }
}

void ui_marquee_reset(void) {
  s_marquee.offset = 0;
  s_marquee.overflow = 0;
  s_marquee.hold = MARQUEE_HOLD_START_TICKS;
}

void ui_draw_menu_row(GContext *ctx, const Layer *cell_layer,
                      const char *text) {
  const bool highlighted = menu_cell_layer_is_highlighted(cell_layer);
  const GRect bounds = layer_get_bounds(cell_layer);

  // Bold for the selected row, exactly as styles.itemSelected did under Piu.
  GFont font = fonts_get_system_font(highlighted ? FONT_KEY_GOTHIC_28_BOLD
                                                 : FONT_KEY_GOTHIC_28);

  graphics_context_set_text_color(
      ctx, highlighted ? UI_COLOR_HIGHLIGHT_TEXT : UI_COLOR_FOREGROUND);

  const int16_t inset = PBL_IF_ROUND_ELSE(8, 6);
  const int16_t available = bounds.size.w - inset * 2;

  // Measured in a box far wider than the screen, so this is the length of the
  // line as one run rather than what happens to fit. graphics_draw_text has no
  // vertical centring either, hence using the height here as well.
  const GSize size = graphics_text_layout_get_content_size(
      text, font, GRect(0, 0, 2000, bounds.size.h), GTextOverflowModeFill,
      GTextAlignmentLeft);
  const int16_t y = (bounds.size.h - size.h) / 2;

  if (highlighted && size.w > available) {
    // Scrolls, so no ellipsis and left-aligned even on a round screen -- a
    // centred marquee reads as drifting rather than as text being revealed.
    // The cell layer clips, so the part shifted out simply disappears.
    s_marquee.overflow = size.w - available;
    prv_marquee_run();
    graphics_draw_text(ctx, text, font,
                       GRect(inset - s_marquee.offset, y, size.w + 4, size.h),
                       GTextOverflowModeFill, GTextAlignmentLeft, NULL);
    return;
  }

  if (highlighted)
    s_marquee.overflow = 0;

  graphics_draw_text(ctx, text, font, GRect(inset, y, available, size.h),
                     GTextOverflowModeTrailingEllipsis,
                     PBL_IF_ROUND_ELSE(GTextAlignmentCenter,
                                       GTextAlignmentLeft),
                     NULL);
}

void ui_draw_menu_header(GContext *ctx, const Layer *cell_layer,
                         const char *text) {
  const GRect bounds = layer_get_bounds(cell_layer);
  GFont font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);

  graphics_context_set_text_color(ctx, UI_COLOR_MUTED);
  graphics_draw_text(ctx, text, font,
                     GRect(6, 0, bounds.size.w - 12, bounds.size.h),
                     GTextOverflowModeTrailingEllipsis,
                     PBL_IF_ROUND_ELSE(GTextAlignmentCenter, GTextAlignmentLeft),
                     NULL);
}
