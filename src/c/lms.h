#pragma once

#include <pebble.h>

/**
 * LMS operations, one step above the raw relay.
 *
 * Everything here turns the field-separated strings from src/pkjs/index.js into
 * structs. Nothing in the UI should have to know about separators.
 *
 * Handlers run on the app's event loop. Pointers passed to them are valid only
 * for the duration of the call -- copy what you need to keep.
 */

#define LMS_NAME_LEN   32
#define LMS_TEXT_LEN   32
#define LMS_TRACK_LEN  48

/**
 * Player ids are MAC addresses, but menu item ids are whatever LMS says --
 * plugins produce long ones, and the phone falls back to "_<parentId>_<n>",
 * which nests. Truncating one silently breaks the lookup in cachedMenu on the
 * phone and the item just answers "item not found", so this is deliberately
 * roomy.
 */
#define LMS_ID_LEN     64

/** Players a single request can report. LMS setups this large are unusual. */
#define LMS_MAX_PLAYERS 8

/**
 * Items per menu request.
 *
 * A page has to survive the trip in one AppMessage (MAX_DATA in
 * src/pkjs/index.js), so this is a transport limit rather than a display one --
 * the visible list is usually shorter, and longer lists page as you scroll.
 */
#define LMS_PAGE 10

typedef struct {
  char name[LMS_NAME_LEN];
  char id[LMS_ID_LEN];
  bool playing;
} LMSPlayer;

typedef struct {
  char artist[LMS_TRACK_LEN];
  char title[LMS_TRACK_LEN];
  int volume;
  bool playing;
} LMSStatus;

/**
 * What selecting an item does. Decided on the phone, where the LMS response's
 * `base.actions` is available -- see classify() in src/pkjs/index.js.
 */
typedef enum {
  LMSItemNone = 0,  //!< nothing to do
  LMSItemNode,      //!< a home-menu section; the phone pages it
  LMSItemCmd,       //!< opens a list; LMS pages it
  LMSItemPlay,      //!< starts playback
} LMSItemKind;

typedef struct {
  char text[LMS_TEXT_LEN];
  char id[LMS_ID_LEN];
  LMSItemKind kind;
} LMSItem;

typedef struct {
  int total;               //!< items at this level, from the server
  int count;               //!< items actually in `items`
  LMSItem items[LMS_PAGE];
} LMSPage;

/** What the phone has ready to send after a lms_cover() request. */
typedef struct {
  int width;
  int height;
  uint32_t byte_length;  //!< ARGB2222, one byte per pixel
  int chunks;
} LMSCover;

typedef void (*LMSCoverHandler)(const char *err, const LMSCover *cover,
                                void *context);

typedef void (*LMSPlayersHandler)(const char *err, const LMSPlayer *players,
                                  int count, void *context);
typedef void (*LMSStatusHandler)(const char *err, const LMSStatus *status,
                                 void *context);
typedef void (*LMSPageHandler)(const char *err, const LMSPage *page,
                               void *context);
typedef void (*LMSDoneHandler)(const char *err, void *context);

void lms_players(LMSPlayersHandler handler, void *context);
void lms_status(const char *player_id, LMSStatusHandler handler, void *context);

/** `command` is a space-separated slim.request, e.g. "mixer volume +5". */
void lms_command(const char *player_id, const char *command,
                 LMSDoneHandler handler, void *context);

/**
 * Fetches one page of a menu level.
 *
 * `node` selects between the two request types the phone understands: true for
 * a home-menu section (`req_id` is the node name, "home" at the top), false for
 * a list reached through an item's `go` action (`req_id` is that item's id).
 */
void lms_menu(const char *player_id, int start, bool node, const char *req_id,
              LMSPageHandler handler, void *context);

/** Runs an item's action -- for LMSItemPlay, starts playback. */
void lms_menu_go(const char *player_id, const char *item_id,
                 LMSDoneHandler handler, void *context);

/**
 * Asks for the current track's cover, scaled server-side to `edge` square.
 *
 * The handler runs once the phone has the image decoded and ready, and reports
 * how much is coming. The pixels themselves arrive afterwards through the
 * chunk handler registered with comm_set_image_handler().
 */
void lms_cover(const char *player_id, int edge, LMSCoverHandler handler,
               void *context);

/**
 * Forgets every request made with this context.
 *
 * A window must call this as it unloads, or a late reply runs a handler against
 * freed state.
 */
void lms_cancel(void *context);
