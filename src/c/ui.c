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
  const GTextAlignment alignment =
      PBL_IF_ROUND_ELSE(GTextAlignmentCenter, GTextAlignmentLeft);

  GRect box = GRect(inset, 0, bounds.size.w - inset * 2, bounds.size.h);

  // graphics_draw_text has no vertical centring, so measure and offset. Doing
  // it by measurement rather than a fixed nudge keeps the row centred if the
  // font ever changes.
  const GSize size = graphics_text_layout_get_content_size(
      text, font, box, GTextOverflowModeTrailingEllipsis, alignment);
  box.origin.y = (bounds.size.h - size.h) / 2;
  box.size.h = size.h;

  graphics_draw_text(ctx, text, font, box, GTextOverflowModeTrailingEllipsis,
                     alignment, NULL);
}

void ui_draw_menu_header(GContext *ctx, const Layer *cell_layer,
                         const char *text) {
  const GRect bounds = layer_get_bounds(cell_layer);
  GFont font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);

  graphics_context_set_text_color(ctx, UI_COLOR_HIGHLIGHT);
  graphics_draw_text(ctx, text, font,
                     GRect(6, 0, bounds.size.w - 12, bounds.size.h),
                     GTextOverflowModeTrailingEllipsis,
                     PBL_IF_ROUND_ELSE(GTextAlignmentCenter, GTextAlignmentLeft),
                     NULL);
}
