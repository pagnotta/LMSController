#include <pebble.h>
#include "comm.h"
#include "ui.h"

/**
 * LMS Controller for Pebble Time 2 (emery) and Pebble Round 2 (gabbro).
 *
 * The watch does the UI; every request to the Logitech Media Server is made by
 * the phone (src/pkjs/index.js) and comes back as compact, field-separated
 * text. See docs/networking.md.
 */

int main(void) {
  comm_init();
  players_window_push();
  app_event_loop();
  comm_deinit();
}
