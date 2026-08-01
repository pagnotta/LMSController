#include "ui.h"

/**
 * Screen 3: the LMS menu tree.
 *
 * One window per level, pushed on top of each other, so Back steps out of a
 * folder without any history of our own. The deepest sensible LMS path is far
 * short of MAX_LEVELS.
 *
 * Paging: MenuLayer is told the level's true row count, taken from the server,
 * while only one page of items is actually in memory. Rows outside that page
 * draw as a placeholder until they arrive. Deducing the end of a list from a
 * short page instead would be wrong -- the phone cuts pages on a record
 * boundary to fit one AppMessage, so a full level can arrive short.
 */

#define MAX_LEVELS 10

typedef struct BrowseWindow {
  Window *window;
  MenuLayer *menu;

  char player_id[LMS_ID_LEN];
  char req_id[LMS_ID_LEN];
  bool node;                 //!< request type for this level
  char title[LMS_TEXT_LEN];

  LMSPage page;              //!< the slice currently in memory
  int page_start;            //!< index of page.items[0] within the level
  int requested_start;       //!< start of the fetch in flight
  bool have_total;
  bool loading;
  char message[40];          //!< shown while the level has no rows yet
} BrowseWindow;

static BrowseWindow *s_levels[MAX_LEVELS];
static int s_level_count;

static void prv_push(const char *player_id, const char *req_id, bool node,
                     const char *title);
static void prv_load_page(BrowseWindow *state, int start);

/** True when `row` is one of the items currently in memory. */
static bool prv_row_loaded(const BrowseWindow *state, int row) {
  return state->have_total && row >= state->page_start &&
         row < state->page_start + state->page.count;
}

/**
 * Fetches the page holding the selection, if it is not already there.
 *
 * Called both when the selection moves and after a page lands: scrolling
 * quickly past a pending request would otherwise leave the cursor parked on
 * rows nobody ever asked for.
 */
static void prv_ensure_selection_loaded(BrowseWindow *state) {
  if (state->loading || !state->have_total)
    return;

  const int row = menu_layer_get_selected_index(state->menu).row;
  if (prv_row_loaded(state, row))
    return;

  prv_load_page(state, (row / LMS_PAGE) * LMS_PAGE);
}

static void prv_on_page(const char *err, const LMSPage *page, void *ctx) {
  BrowseWindow *state = ctx;
  state->loading = false;

  if (err) {
    snprintf(state->message, sizeof(state->message), "Error: %s", err);
    if (!state->have_total)
      menu_layer_reload_data(state->menu);
    return;
  }

  // Only now does page_start become true. Committing it at request time would
  // make a failed fetch claim the old items belong to the new range.
  state->page_start = state->requested_start;
  state->page = *page;
  state->have_total = true;
  if (page->total == 0)
    strncpy(state->message, "Empty", sizeof(state->message) - 1);

  menu_layer_reload_data(state->menu);
  prv_ensure_selection_loaded(state);
}

static void prv_load_page(BrowseWindow *state, int start) {
  state->loading = true;
  state->requested_start = start;
  lms_menu(state->player_id, start, state->node, state->req_id, prv_on_page,
           state);
}

// --- menu layer ------------------------------------------------------------

static uint16_t prv_num_rows(MenuLayer *menu, uint16_t section, void *ctx) {
  BrowseWindow *state = ctx;
  if (!state->have_total || state->page.total == 0)
    return 1;  // the message row
  return state->page.total;
}

static int16_t prv_header_height(MenuLayer *menu, uint16_t section, void *ctx) {
  return MENU_CELL_BASIC_HEADER_HEIGHT;
}

static void prv_draw_header(GContext *gctx, const Layer *cell, uint16_t section,
                            void *ctx) {
  BrowseWindow *state = ctx;
  menu_cell_basic_header_draw(gctx, cell, state->title);
}

static void prv_draw_row(GContext *gctx, const Layer *cell, MenuIndex *index,
                         void *ctx) {
  BrowseWindow *state = ctx;

  if (!state->have_total || state->page.total == 0) {
    menu_cell_basic_draw(gctx, cell, state->message, NULL, NULL);
    return;
  }
  if (!prv_row_loaded(state, index->row)) {
    menu_cell_basic_draw(gctx, cell, "...", NULL, NULL);
    return;
  }

  const LMSItem *item = &state->page.items[index->row - state->page_start];
  menu_cell_basic_draw(gctx, cell, item->text, NULL, NULL);
}

static void prv_on_selection_changed(MenuLayer *menu, MenuIndex new_index,
                                     MenuIndex old_index, void *ctx) {
  prv_ensure_selection_loaded(ctx);
}

static void prv_on_played(const char *err, void *ctx) {
  BrowseWindow *state = ctx;
  if (err) {
    snprintf(state->message, sizeof(state->message), "Error: %s", err);
    menu_layer_reload_data(state->menu);
    return;
  }
  // Playback started -- the interesting screen is now the player, not the
  // folder the track happened to live in.
  browse_close_all();
}

static void prv_select(MenuLayer *menu, MenuIndex *index, void *ctx) {
  BrowseWindow *state = ctx;
  if (!prv_row_loaded(state, index->row))
    return;

  const LMSItem *item = &state->page.items[index->row - state->page_start];

  switch (item->kind) {
    case LMSItemNode:
    case LMSItemCmd:
      prv_push(state->player_id, item->id, item->kind == LMSItemNode,
               item->text);
      break;
    case LMSItemPlay:
      lms_menu_go(state->player_id, item->id, prv_on_played, state);
      break;
    case LMSItemNone:
      break;
  }
}

// --- window ----------------------------------------------------------------

static void prv_window_load(Window *window) {
  BrowseWindow *state = window_get_user_data(window);
  Layer *root = window_get_root_layer(window);

  state->menu = menu_layer_create(layer_get_bounds(root));
  menu_layer_set_callbacks(state->menu, state, (MenuLayerCallbacks){
      .get_num_rows = prv_num_rows,
      .get_header_height = prv_header_height,
      .draw_header = prv_draw_header,
      .draw_row = prv_draw_row,
      .select_click = prv_select,
      .selection_changed = prv_on_selection_changed,
  });
  ui_style_menu_layer(state->menu);
  menu_layer_set_click_config_onto_window(state->menu, window);
  layer_add_child(root, menu_layer_get_layer(state->menu));

  prv_load_page(state, 0);
}

static void prv_window_unload(Window *window) {
  BrowseWindow *state = window_get_user_data(window);
  lms_cancel(state);

  for (int i = 0; i < s_level_count; i++) {
    if (s_levels[i] == state) {
      for (int j = i; j < s_level_count - 1; j++)
        s_levels[j] = s_levels[j + 1];
      s_level_count--;
      break;
    }
  }

  menu_layer_destroy(state->menu);
  window_destroy(state->window);
  free(state);
}

static void prv_push(const char *player_id, const char *req_id, bool node,
                     const char *title) {
  if (s_level_count >= MAX_LEVELS)
    return;

  BrowseWindow *state = malloc(sizeof(BrowseWindow));
  if (!state)
    return;
  memset(state, 0, sizeof(BrowseWindow));

  strncpy(state->player_id, player_id, sizeof(state->player_id) - 1);
  strncpy(state->req_id, req_id, sizeof(state->req_id) - 1);
  strncpy(state->title, title, sizeof(state->title) - 1);
  strncpy(state->message, "Loading...", sizeof(state->message) - 1);
  state->node = node;

  state->window = window_create();
  window_set_user_data(state->window, state);
  window_set_background_color(state->window, UI_COLOR_BACKGROUND);
  window_set_window_handlers(state->window, (WindowHandlers){
      .load = prv_window_load,
      .unload = prv_window_unload,
  });

  s_levels[s_level_count++] = state;
  window_stack_push(state->window, true);
}

void browse_window_push_root(const char *player_id) {
  prv_push(player_id, "home", true, "LMS");
}

void browse_close_all(void) {
  // Each removal unloads its window, which takes the level back out of
  // s_levels -- hence the loop condition rather than an index walk.
  int guard = MAX_LEVELS + 1;
  while (s_level_count > 0 && guard-- > 0)
    window_stack_remove(s_levels[s_level_count - 1]->window, false);
}
