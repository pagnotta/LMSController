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

/**
 * Passes before a line gives up and settles back to an ellipsis.
 *
 * Scrolling costs a redraw every 40 ms, and in a MenuLayer that redraws every
 * visible row, not just the one moving. Three passes is enough to read a name
 * twice over; after that it is only draining the battery at whatever rate the
 * app happens to stay open. A new selection or a new track starts the count
 * again.
 */
#define MARQUEE_CYCLES 3

static struct {
  MenuLayer *menu;
  AppTimer *timer;
  int16_t offset;    //!< pixels the text is currently shifted left
  int16_t overflow;  //!< how far it extends past the cell; 0 means no scrolling
  int16_t hold;      //!< ticks left to sit still at one end
  uint8_t cycles;    //!< passes completed
  bool stopped;      //!< done travelling; drawn with an ellipsis from here on
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
  if (!s_marquee.timer && s_marquee.menu && !s_marquee.stopped)
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
    if (++s_marquee.cycles >= MARQUEE_CYCLES)
      s_marquee.stopped = true;
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
  s_marquee.cycles = 0;
  s_marquee.stopped = false;
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

  if (highlighted && size.w > available && !s_marquee.stopped) {
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

// --- MarqueeLabel ----------------------------------------------------------

/**
 * Scrolling single-line labels.
 *
 * A fixed pool rather than malloc: the player screen needs two, and a pool
 * makes the shared timer's bookkeeping trivial -- it walks the array and stops
 * itself as soon as nothing is left that overflows.
 */
#define MARQUEE_LABELS 4

struct MarqueeLabel {
  Layer *layer;
  GFont font;
  GColor color;
  GTextAlignment alignment;
  const char *text;    //!< caller-owned, as with TextLayer
  int16_t offset;
  int16_t overflow;    //!< px past the edge; 0 means it fits and will not move
  int16_t text_height; //!< measured once, in set_text
  int16_t hold;
  uint8_t cycles;
  bool stopped;
  bool used;
};

static struct MarqueeLabel s_labels[MARQUEE_LABELS];
static AppTimer *s_label_timer;

static void prv_label_tick(void *ctx);

static void prv_label_run(void) {
  if (!s_label_timer)
    s_label_timer = app_timer_register(MARQUEE_TICK_MS, prv_label_tick, NULL);
}

static void prv_label_tick(void *ctx) {
  s_label_timer = NULL;
  bool moving = false;

  for (int i = 0; i < MARQUEE_LABELS; i++) {
    MarqueeLabel *label = &s_labels[i];
    if (!label->used || label->overflow <= 0)
      continue;
    moving = true;

    if (label->hold > 0) {
      label->hold--;
    } else if (label->offset < label->overflow) {
      label->offset += MARQUEE_STEP_PX;
      if (label->offset >= label->overflow) {
        label->offset = label->overflow;
        label->hold = MARQUEE_HOLD_END_TICKS;
      }
    } else {
      label->offset = 0;
      label->hold = MARQUEE_HOLD_START_TICKS;
      if (++label->cycles >= MARQUEE_CYCLES) {
        label->stopped = true;
        label->overflow = 0;  // settles back to an ellipsis
      }
    }
    layer_mark_dirty(label->layer);
  }

  if (moving)
    prv_label_run();
}

/**
 * Draws the line. Nothing is measured here -- set_text did that once, so a
 * scrolling line costs a draw per frame and not a text layout as well.
 */
static void prv_label_draw(Layer *layer, GContext *ctx) {
  MarqueeLabel *label = *(MarqueeLabel **)layer_get_data(layer);
  if (!label->text || !label->text[0])
    return;

  const GRect bounds = layer_get_bounds(layer);
  const int16_t y = (bounds.size.h - label->text_height) / 2;
  graphics_context_set_text_color(ctx, label->color);

  if (label->overflow > 0) {
    // Scrolling, so no ellipsis and left-aligned whatever the screen shape --
    // a centred marquee reads as drifting rather than as text being revealed.
    graphics_draw_text(ctx, label->text, label->font,
                       GRect(-label->offset, y,
                             bounds.size.w + label->overflow + 4,
                             label->text_height),
                       GTextOverflowModeFill, GTextAlignmentLeft, NULL);
    return;
  }

  graphics_draw_text(ctx, label->text, label->font,
                     GRect(0, y, bounds.size.w, label->text_height),
                     GTextOverflowModeTrailingEllipsis, label->alignment, NULL);
}

MarqueeLabel *marquee_label_create(GRect frame, const char *font_key,
                                   GColor color, GTextAlignment alignment) {
  MarqueeLabel *label = NULL;
  for (int i = 0; i < MARQUEE_LABELS; i++)
    if (!s_labels[i].used) {
      label = &s_labels[i];
      break;
    }
  if (!label)
    return NULL;

  memset(label, 0, sizeof(*label));
  label->used = true;
  label->font = fonts_get_system_font(font_key);
  label->color = color;
  label->alignment = alignment;

  label->layer = layer_create_with_data(frame, sizeof(MarqueeLabel *));
  if (!label->layer) {
    label->used = false;
    return NULL;
  }
  *(MarqueeLabel **)layer_get_data(label->layer) = label;
  layer_set_update_proc(label->layer, prv_label_draw);
  return label;
}

void marquee_label_destroy(MarqueeLabel *label) {
  if (!label)
    return;
  layer_destroy(label->layer);
  label->used = false;
  label->layer = NULL;
  label->text = NULL;
}

Layer *marquee_label_get_layer(MarqueeLabel *label) {
  return label ? label->layer : NULL;
}

void marquee_label_set_text(MarqueeLabel *label, const char *text) {
  if (!label)
    return;

  label->text = text;
  label->offset = 0;
  label->overflow = 0;
  label->hold = MARQUEE_HOLD_START_TICKS;
  label->cycles = 0;
  label->stopped = false;

  const GRect bounds = layer_get_bounds(label->layer);
  if (text && text[0]) {
    // Measured in a box far wider than the screen, so this is the length of the
    // line as one run rather than what happens to fit. Kept, because the draw
    // runs 25 times a second while the line travels and this does not change.
    const GSize size = graphics_text_layout_get_content_size(
        text, label->font, GRect(0, 0, 2000, bounds.size.h),
        GTextOverflowModeFill, GTextAlignmentLeft);
    label->text_height = size.h;
    if (size.w > bounds.size.w) {
      label->overflow = size.w - bounds.size.w;
      prv_label_run();
    }
  }
  layer_mark_dirty(label->layer);
}
