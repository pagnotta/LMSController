#include "ui.h"

/**
 * Screen 3: the LMS menu tree.
 *
 * One window per level, pushed on top of each other, so Back steps out of a
 * folder without any history of our own. The deepest sensible LMS path is far
 * short of MAX_LEVELS.
 *
 * Paging: MenuLayer is told the level's true row count, taken from the server,
 * while only part of the level is in memory. Deducing the end of a list from a
 * short page instead would be wrong -- the phone cuts pages on a record
 * boundary to fit one AppMessage, so a full level can arrive short.
 *
 * What is in memory is a contiguous run of items that slides as you scroll, not
 * a single page. Holding one page meant every fetch threw away the rows just
 * read, so scrolling left a trail of placeholders both ahead of and behind the
 * cursor. Several pages cost a few kilobytes, which this app has.
 *
 * On top of that the next page is fetched before the cursor reaches it, so in
 * steady scrolling the placeholder is never seen at all.
 */

#define MAX_LEVELS 10

/**
 * Items kept per level. Three pages is enough that the rows behind the cursor
 * survive a fetch ahead of it; at 100 bytes an item that is 3 KB per level.
 */
#define CACHE_PAGES 3
#define CACHE_ITEMS (LMS_PAGE * CACHE_PAGES)

/** How close the cursor may come to an unloaded edge before we fetch. */
#define PREFETCH_MARGIN 4

/**
 * How long to wait before asking again for a page that did not arrive.
 *
 * Scrolling hard through a long level runs the fetch, the prefetch and the
 * status poll into the four request slots the relay has, and the loser comes
 * back "busy". Without a retry the level then sat there with a cache nowhere
 * near the screen, drawing the placeholder in every row until the selection
 * happened to move again.
 */
#define RETRY_MS 700

/**
 * Rows MenuLayer is told about at once.
 *
 * ScrollLayer keeps its content size in a GSize, whose fields are int16_t, so a
 * list taller than 32767 px cannot be represented at all. At 42 px a row that
 * is 780 rows; past it the height wraps negative, the layer decides there is
 * nothing to scroll, and the selection simply walks off the bottom of the
 * screen while the rows stay put. A library with 2770 albums hits this from the
 * very first row.
 *
 * So the level is shown through a window of this many rows, which moves along
 * when the cursor reaches either end. 600 leaves room on both screens: 25200 px
 * on emery, and on gabbro 24028 with its one taller focused row.
 */
#define MAX_MENU_ROWS 600

typedef struct BrowseWindow {
  Window *window;
  MenuLayer *menu;

  char player_id[LMS_ID_LEN];
  char req_id[LMS_ID_LEN];
  bool node;                 //!< request type for this level
  char title[LMS_TEXT_LEN];

  LMSItem cache[CACHE_ITEMS];
  int cache_start;           //!< index of cache[0] within the level
  int cache_count;
  int total;                 //!< rows at this level, from the server
  int row_offset;            //!< level index shown as MenuLayer row 0

  int requested_start;       //!< start of the fetch in flight
  AppTimer *retry_timer;     //!< set after a fetch that did not come back
  uint16_t restore_row;      //!< selection to reinstate once the level loads
  bool restored;             //!< came from the saved path, not from a tap
  bool have_total;
  bool loading;
  char message[40];          //!< shown while the level has no rows yet
} BrowseWindow;

static BrowseWindow *s_levels[MAX_LEVELS];
static int s_level_count;

/**
 * How far back the saved path reaches: to the deepest level that still holds
 * folders.
 *
 * A count of levels cannot work, because the depth playback is started from is
 * not fixed. With something already playing, opening an album gives an options
 * menu first -- play after this track, play everything now -- so the path is
 * one level longer than when nothing is playing and a track starts straight
 * away.
 *
 * What both have in common is the shape: a track list and an options menu
 * consist only of things to play, while the album list above them is the last
 * level anything can be navigated from. That is where the user wants to be, and
 * kind already says which is which.
 *
 * This runs as playback closes the menu, so it reads the levels that were
 * actually walked rather than guessing a depth from what is playing later.
 */
static bool prv_level_has_folders(const BrowseWindow *level) {
  for (int i = 0; i < level->cache_count; i++)
    if (level->cache[i].kind == LMSItemNode || level->cache[i].kind == LMSItemCmd)
      return true;
  return false;
}

/**
 * Where the user was when playback took them away.
 *
 * Starting a track closes the whole menu, and coming back at the root meant
 * walking the tree again just to reach the album next to the one that was
 * picked -- which is the commonest thing to want.
 *
 * Only remembered across that jump, not across leaving the menu by hand: if
 * the user walked out level by level, they meant to leave. And only within one
 * run of the app -- these ids are keys into cachedMenu on the phone, which is
 * torn down with the app, so a path from a previous run would resolve to
 * nothing. A level that fails to load throws the path away and leaves the user
 * where they are.
 */
typedef struct {
  char req_id[LMS_ID_LEN];
  char title[LMS_TEXT_LEN];
  bool node;
  uint16_t selected;
} SavedLevel;

static SavedLevel s_saved[MAX_LEVELS];
static int s_saved_depth;
static char s_saved_player[LMS_ID_LEN];

// Set while browse_close_all() is unwinding, so the unload handler can tell a
// jump to the player from the user backing out of the root by hand.
static bool s_closing;

/**
 * Set while a saved path is being pushed back on.
 *
 * Levels normally fetch their first page as they load. Restoring pushes the
 * whole path at once, so that fired one request per level simultaneously and
 * ran the relay out of slots -- four, between lms.c and comm.c -- which came
 * back as "busy" on reopening. Only the level the user ends up looking at is
 * fetched now; the ones underneath load if and when Back reaches them.
 */
static bool s_restoring;

static void prv_push(const char *player_id, const char *req_id, bool node,
                     const char *title, uint16_t selected, bool restored);
static void prv_load_page(BrowseWindow *state, int start);
static void prv_shift_window(BrowseWindow *state, int wanted_index);

static void prv_forget_path(void) {
  s_saved_depth = 0;
  s_saved_player[0] = '\0';
}

/** MenuLayer counts rows from the window's start; the level does not. */
static int prv_level_index(const BrowseWindow *state, int row) {
  return state->row_offset + row;
}

/** Rows in the window: the rest of the level, up to the window's size. */
static uint16_t prv_window_rows(const BrowseWindow *state) {
  const int left = state->total - state->row_offset;
  if (left <= 0)
    return 0;
  return left < MAX_MENU_ROWS ? (uint16_t)left : MAX_MENU_ROWS;
}

/** True when this level index is one of the items currently in memory. */
static bool prv_index_loaded(const BrowseWindow *state, int index) {
  return state->have_total && index >= state->cache_start &&
         index < state->cache_start + state->cache_count;
}

static int prv_cache_end(const BrowseWindow *state) {
  return state->cache_start + state->cache_count;
}

static void prv_cache_reset(BrowseWindow *state, int start,
                            const LMSItem *items, int count) {
  memcpy(state->cache, items, sizeof(LMSItem) * count);
  state->cache_start = start;
  state->cache_count = count;
}

/**
 * Folds an arriving page into the cached run.
 *
 * Only a page that touches the run can extend it; anything else is a jump, and
 * the run restarts there. Overflow is dropped from the far end, so the items
 * nearest the cursor are the ones that survive.
 */
static void prv_cache_merge(BrowseWindow *state, int start,
                            const LMSItem *items, int count) {
  if (count <= 0)
    return;
  if (state->cache_count == 0 || count >= CACHE_ITEMS) {
    prv_cache_reset(state, start, items, count);
    return;
  }

  if (start == prv_cache_end(state)) {
    const int overflow = state->cache_count + count - CACHE_ITEMS;
    if (overflow > 0) {
      memmove(state->cache, state->cache + overflow,
              sizeof(LMSItem) * (state->cache_count - overflow));
      state->cache_start += overflow;
      state->cache_count -= overflow;
    }
    memcpy(state->cache + state->cache_count, items, sizeof(LMSItem) * count);
    state->cache_count += count;
    return;
  }

  if (start + count == state->cache_start) {
    const int overflow = state->cache_count + count - CACHE_ITEMS;
    if (overflow > 0)
      state->cache_count -= overflow;  // drop the far end
    memmove(state->cache + count, state->cache,
            sizeof(LMSItem) * state->cache_count);
    memcpy(state->cache, items, sizeof(LMSItem) * count);
    state->cache_start = start;
    state->cache_count += count;
    return;
  }

  if (start >= state->cache_start && start + count <= prv_cache_end(state)) {
    memcpy(state->cache + (start - state->cache_start), items,
           sizeof(LMSItem) * count);
    return;
  }

  prv_cache_reset(state, start, items, count);
}

/**
 * Keeps the run around the cursor filled.
 *
 * Three jobs: fetch the page the cursor landed on if it is not there, and pull
 * in the next or previous page once the cursor comes within PREFETCH_MARGIN of
 * an edge. Called when the selection moves and again after a page lands --
 * scrolling past a pending request would otherwise leave the cursor parked on
 * rows nobody ever asked for.
 *
 * The two prefetch directions are split at the middle of the run so they cannot
 * take turns evicting each other's items on a full cache.
 */
static void prv_ensure_selection_loaded(BrowseWindow *state) {
  if (state->loading || !state->have_total || state->total == 0)
    return;

  const int row = prv_level_index(state,
                                  menu_layer_get_selected_index(state->menu).row);

  if (!prv_index_loaded(state, row)) {
    prv_load_page(state, (row / LMS_PAGE) * LMS_PAGE);
    return;
  }

  const int end = prv_cache_end(state);
  const int middle = state->cache_start + state->cache_count / 2;

  if (row >= middle) {
    if (row + PREFETCH_MARGIN >= end && end < state->total)
      prv_load_page(state, end);
    return;
  }

  if (row - PREFETCH_MARGIN < state->cache_start && state->cache_start > 0) {
    const int start = state->cache_start - LMS_PAGE;
    prv_load_page(state, start < 0 ? 0 : start);
  }
}

static void prv_on_retry(void *ctx) {
  BrowseWindow *state = ctx;
  state->retry_timer = NULL;
  prv_ensure_selection_loaded(state);
}

static void prv_on_page(const char *err, const LMSPage *page, void *ctx) {
  BrowseWindow *state = ctx;
  state->loading = false;

  if (err) {
    snprintf(state->message, sizeof(state->message), "Error: %s", err);
    if (!state->have_total)
      menu_layer_reload_data(state->menu);
    // A restored level that will not load means the phone no longer knows
    // these ids. Keeping the path would fail the same way every time.
    if (state->restored)
      prv_forget_path();
    else if (!state->retry_timer)
      state->retry_timer = app_timer_register(RETRY_MS, prv_on_retry, state);
    return;
  }

  // Committed only now: a failed fetch must not make the cached items claim
  // the range it asked for.
  prv_cache_merge(state, state->requested_start, page->items, page->count);
  state->total = page->total;
  state->have_total = true;
  if (page->total == 0)
    strncpy(state->message, "Empty", sizeof(state->message) - 1);

  // Keep the selection before reloading: reload_data lays the list out afresh
  // and leaves the scroll offset where it was, so on a long level the highlight
  // walked off the bottom of the screen while the rows stayed put.
  MenuIndex selected = menu_layer_get_selected_index(state->menu);

  // Reinstating a saved row has to wait for the total: before that the list has
  // one message row and any index would be clamped to it. A saved row is a
  // level index, so it may need the window moved to reach it.
  if (state->restore_row) {
    const int wanted = state->restore_row;
    state->restore_row = 0;
    if (wanted >= MAX_MENU_ROWS) {
      prv_shift_window(state, wanted);
      return;
    }
    selected = MenuIndex(0, wanted);
  }

  menu_layer_reload_data(state->menu);
  menu_layer_set_selected_index(state->menu, selected, MenuRowAlignCenter, false);

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
  if (!state->have_total || state->total == 0)
    return 1;  // the message row
  return prv_window_rows(state);
}

static int16_t prv_header_height(MenuLayer *menu, uint16_t section, void *ctx) {
  return ui_menu_header_height();
}

static int16_t prv_cell_height(MenuLayer *menu, MenuIndex *index, void *ctx) {
  const MenuIndex selected = menu_layer_get_selected_index(menu);
  return ui_menu_cell_height(menu_index_compare(&selected, index) == 0);
}

static void prv_draw_header(GContext *gctx, const Layer *cell, uint16_t section,
                            void *ctx) {
  BrowseWindow *state = ctx;
  ui_draw_menu_header(gctx, cell, state->title);
}

static void prv_draw_row(GContext *gctx, const Layer *cell, MenuIndex *index,
                         void *ctx) {
  BrowseWindow *state = ctx;

  if (!state->have_total || state->total == 0) {
    ui_draw_menu_row(gctx, cell, state->message);
    return;
  }
  const int level_index = prv_level_index(state, index->row);
  if (!prv_index_loaded(state, level_index)) {
    ui_draw_menu_row(gctx, cell, "...");
    return;
  }

  // "> " marks a folder, so a list mixing albums and tracks reads at a glance.
  const LMSItem *item = &state->cache[level_index - state->cache_start];
  const bool folder = item->kind == LMSItemNode || item->kind == LMSItemCmd;
  char line[LMS_TEXT_LEN + 3];
  snprintf(line, sizeof(line), "%s%s", folder ? "> " : "", item->text);
  ui_draw_menu_row(gctx, cell, line);
}

static void prv_on_selection_changed(MenuLayer *menu, MenuIndex new_index,
                                     MenuIndex old_index, void *ctx) {
  ui_marquee_reset();
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
  const int level_index = prv_level_index(state, index->row);
  if (!prv_index_loaded(state, level_index))
    return;

  const LMSItem *item = &state->cache[level_index - state->cache_start];

  switch (item->kind) {
    case LMSItemNode:
    case LMSItemCmd:
      prv_push(state->player_id, item->id, item->kind == LMSItemNode,
               item->text, 0, false);
      break;
    case LMSItemPlay:
      lms_menu_go(state->player_id, item->id, prv_on_played, state);
      break;
    case LMSItemNone:
      break;
  }
}

// --- buttons ---------------------------------------------------------------

static void prv_close_all(bool trim_to_folders);

/**
 * Moves the window along and puts the cursor back where it was heading.
 *
 * Only ever at an end of the window, so the jump in the visible rows happens
 * exactly where the user is already pushing against a boundary. Half a window
 * at a time, so the same edge is not hit again immediately.
 */
static void prv_shift_window(BrowseWindow *state, int wanted_index) {
  int offset = wanted_index - MAX_MENU_ROWS / 2;
  if (offset > state->total - MAX_MENU_ROWS)
    offset = state->total - MAX_MENU_ROWS;
  if (offset < 0)
    offset = 0;
  if (offset == state->row_offset)
    return;

  state->row_offset = offset;
  menu_layer_reload_data(state->menu);
  menu_layer_set_selected_index(state->menu,
                                MenuIndex(0, wanted_index - offset),
                                MenuRowAlignCenter, false);
  prv_ensure_selection_loaded(state);
}

static void prv_click_up(ClickRecognizerRef recognizer, void *ctx) {
  BrowseWindow *state = ctx;
  const int row = menu_layer_get_selected_index(state->menu).row;

  if (row == 0 && state->row_offset > 0) {
    prv_shift_window(state, prv_level_index(state, row) - 1);
    return;
  }
  menu_layer_set_selected_next(state->menu, true, MenuRowAlignCenter, true);
}

static void prv_click_down(ClickRecognizerRef recognizer, void *ctx) {
  BrowseWindow *state = ctx;
  const int row = menu_layer_get_selected_index(state->menu).row;

  if (row + 1 >= prv_window_rows(state) &&
      prv_level_index(state, row) + 1 < state->total) {
    prv_shift_window(state, prv_level_index(state, row) + 1);
    return;
  }
  menu_layer_set_selected_next(state->menu, false, MenuRowAlignCenter, true);
}

static void prv_click_select(ClickRecognizerRef recognizer, void *ctx) {
  BrowseWindow *state = ctx;
  MenuIndex index = menu_layer_get_selected_index(state->menu);
  prv_select(state->menu, &index, state);
}

/**
 * Hold Select to leave the tree in one go.
 *
 * Five levels down, getting back to the player meant five presses of Back if
 * nothing had been started. The whole path is kept, not trimmed to the last
 * folder level: nothing was played, so the user was in the middle of something
 * and should come back to exactly where they stood.
 */
static void prv_click_select_long(ClickRecognizerRef recognizer, void *ctx) {
  prv_close_all(false);
}

/**
 * Built by hand rather than menu_layer_set_click_config_onto_window, which
 * claims the whole window and leaves no room for the long press. Up and Down
 * do what that function would have bound them to.
 */
static void prv_click_config(void *ctx) {
  window_single_repeating_click_subscribe(BUTTON_ID_UP, 100, prv_click_up);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 100, prv_click_down);
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_click_select);
  window_long_click_subscribe(BUTTON_ID_SELECT, 700, prv_click_select_long, NULL);
}

// --- window ----------------------------------------------------------------

static void prv_window_load(Window *window) {
  BrowseWindow *state = window_get_user_data(window);
  Layer *root = window_get_root_layer(window);

  state->menu = menu_layer_create(layer_get_bounds(root));
  menu_layer_set_callbacks(state->menu, state, (MenuLayerCallbacks){
      .get_num_rows = prv_num_rows,
      .get_cell_height = prv_cell_height,
      .get_header_height = prv_header_height,
      .draw_header = prv_draw_header,
      .draw_row = prv_draw_row,
      .select_click = prv_select,
      .selection_changed = prv_on_selection_changed,
  });
  ui_style_menu_layer(state->menu);
  window_set_click_config_provider_with_context(window, prv_click_config, state);
  layer_add_child(root, menu_layer_get_layer(state->menu));

  if (!s_restoring)
    prv_load_page(state, 0);
}

static void prv_window_appear(Window *window) {
  BrowseWindow *state = window_get_user_data(window);
  ui_marquee_set_menu(state->menu);

  // A restored level below the top has nothing yet; this is where it gets it,
  // once Back actually brings the user to it.
  if (!s_restoring && !state->have_total && !state->loading)
    prv_load_page(state, 0);
}

static void prv_window_disappear(Window *window) {
  ui_marquee_set_menu(NULL);
}

static void prv_window_unload(Window *window) {
  BrowseWindow *state = window_get_user_data(window);
  lms_cancel(state);
  if (state->retry_timer)
    app_timer_cancel(state->retry_timer);

  for (int i = 0; i < s_level_count; i++) {
    if (s_levels[i] == state) {
      for (int j = i; j < s_level_count - 1; j++)
        s_levels[j] = s_levels[j + 1];
      s_level_count--;
      break;
    }
  }

  // Backing out of the root by hand means the user meant to leave; only the
  // jump to the player is worth resuming.
  if (!s_closing && s_level_count == 0)
    prv_forget_path();

  menu_layer_destroy(state->menu);
  window_destroy(state->window);
  free(state);
}

static void prv_push(const char *player_id, const char *req_id, bool node,
                     const char *title, uint16_t selected, bool restored) {
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
  state->restore_row = selected;
  state->restored = restored;

  state->window = window_create();
  window_set_user_data(state->window, state);
  window_set_background_color(state->window, UI_COLOR_BACKGROUND);
  window_set_window_handlers(state->window, (WindowHandlers){
      .load = prv_window_load,
      .appear = prv_window_appear,
      .disappear = prv_window_disappear,
      .unload = prv_window_unload,
  });

  s_levels[s_level_count++] = state;
  window_stack_push(state->window, true);
}

void browse_window_push_root(const char *player_id) {
  // A path belongs to the player it was walked for.
  if (s_saved_depth > 0 && strcmp(s_saved_player, player_id) != 0)
    prv_forget_path();

  if (s_saved_depth == 0) {
    prv_push(player_id, "home", true, "LMS", 0, false);
    return;
  }

  // Rebuilt from the root up, so Back still walks out the way it came in.
  const int depth = s_saved_depth;
  SavedLevel path[MAX_LEVELS];
  memcpy(path, s_saved, sizeof(SavedLevel) * depth);
  prv_forget_path();

  s_restoring = true;
  for (int i = 0; i < depth; i++)
    prv_push(player_id, path[i].req_id, path[i].node, path[i].title,
             path[i].selected, true);
  s_restoring = false;

  if (s_level_count > 0)
    prv_load_page(s_levels[s_level_count - 1], 0);
}

/**
 * Closes the tree, remembering the way back.
 *
 * `trim_to_folders` separates the two ways out. Playback jumps out of a track
 * list or an options menu, and coming back to one of those would be useless --
 * see prv_level_has_folders. Leaving by hand means the user was still looking
 * around, so the path is kept whole.
 */
static void prv_close_all(bool trim_to_folders) {
  // Snapshot before unwinding: coming back at the root is exactly what makes
  // picking a second album tedious.
  int keep = s_level_count < MAX_LEVELS ? s_level_count : MAX_LEVELS;
  if (trim_to_folders) {
    keep = 0;
    for (int i = 0; i < s_level_count && i < MAX_LEVELS; i++)
      if (prv_level_has_folders(s_levels[i]))
        keep = i + 1;
  }

  s_saved_depth = 0;
  for (int i = 0; i < keep; i++) {
    BrowseWindow *level = s_levels[i];
    strncpy(s_saved[i].req_id, level->req_id, sizeof(s_saved[i].req_id) - 1);
    s_saved[i].req_id[sizeof(s_saved[i].req_id) - 1] = '\0';
    strncpy(s_saved[i].title, level->title, sizeof(s_saved[i].title) - 1);
    s_saved[i].title[sizeof(s_saved[i].title) - 1] = '\0';
    s_saved[i].node = level->node;
    s_saved[i].selected =
        prv_level_index(level, menu_layer_get_selected_index(level->menu).row);
    s_saved_depth++;
  }
  if (s_saved_depth > 0) {
    strncpy(s_saved_player, s_levels[0]->player_id, sizeof(s_saved_player) - 1);
    s_saved_player[sizeof(s_saved_player) - 1] = '\0';
  }

  // Each removal unloads its window, which takes the level back out of
  // s_levels -- hence the loop condition rather than an index walk.
  s_closing = true;
  int guard = MAX_LEVELS + 1;
  while (s_level_count > 0 && guard-- > 0)
    window_stack_remove(s_levels[s_level_count - 1]->window, false);
  s_closing = false;
}

void browse_close_all(void) {
  prv_close_all(true);
}
