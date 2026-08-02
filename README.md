# LMSController

Control your Logitech Media Server players from your Pebble.

This branch (`c-port`) is the rewrite in **C** against Pebble SDK 4.17, for
**emery** (Pebble Time 2) and **gabbro** (Pebble Round 2).

The original Pebble.js version is untouched on `master`. The Alloy/Piu attempt
lives on `alloy` — it works, but it cannot display cover art at all, which is
why the app moved to C. The reasoning and the measurements are in
[docs/cover-art.md](docs/cover-art.md).

## Build and run

```bash
pebble build
pebble install --emulator emery      # or gabbro
pebble install --phone <phone-ip>    # real watch
```

Tested with pebble-tool 5.0.39 and SDK 4.17.

## Layout

```
src/
  c/
    main.c              app entry
    comm.c              AppMessage relay to the phone, queue and timeouts
    lms.c               LMS operations; turns wire strings into structs
    players_window.c    screen 1: player list
    status_window.c     screen 2: now playing, touch control
    browse_window.c     screen 3: the LMS menu tree, one window per level
    ui.h                colours and the screen entry points
  pkjs/
    index.js            HTTP to LMS, reduces responses for the watch
```

Networking deliberately goes through the phone rather than from the watch. The
reasons and measurements are in [docs/networking.md](docs/networking.md).

`src/pkjs/index.js` carries over from the Alloy version — the wire protocol is
the same, and it is the half that took the most debugging. Cover art is the one
addition.

## Cover art

The watch measures the space its layout leaves above the text and asks for a
square that size. The phone has LMS scale the image server-side
(`/music/current/cover_<n>x<n>_p.jpg?player=<id>`), which keeps the download
under 4 KB and the decode cheap, unpacks the JPEG with `jpeg-js`, and quantises
to ARGB2222 — one byte per pixel, two bits per channel.

The reply to the request carries the dimensions, so a failed download is
reported like any other error. The pixels then follow as chunks the phone
pushes on its own, each sent from the success callback of the last, so the
firmware's acknowledgement paces the transfer. The watch copies them straight
into a `gbitmap_create_blank()` buffer at the byte offset each chunk names.

`jpeg-js` is pinned to 0.3.x on purpose: 0.4 uses object spread and `const`,
and the webpack the SDK bundles is version 1, which parses neither.

## Controls

Following [navigation_concept.md](navigation_concept.md):

| Screen | Buttons | Touch |
| --- | --- | --- |
| Player list | Up/Down select, Select opens, Back exits | — |
| Now playing | Select opens the menu, Up/Down volume, Back returns | tap play/pause, swipe left/right track, swipe up/down volume |
| LMS menu | Up/Down scroll, Select opens or plays, Back one level up | — |

## Memory

Measured on a physical Pebble Time 2: the app slot is **122568 bytes**, of which
this build leaves about 123 KB free at startup. The same app under Alloy had
12 KB free — the XS machine, the mod archive and the JS runtime take the rest.

That difference is why cover art is possible here and was not there. An
uncompressed ARGB2222 cover costs `width * height` bytes: 33200 for the 200x166
the reference app uses on emery, 67600 for gabbro at 260x260.

## Status

Working: player list, now playing with artist, title, volume and playback
state, touch gestures, browsing the full LMS menu tree with paging, and
starting playback from it.

Not yet done: cover art (next, into the space above the artist line on the now
playing screen), AppGlance, voice search.

## History

Originally by Christian Herzog (daduke). The Pebble.js version on `master` dates
from 2016 and ran unchanged for ten years.

## License

See [LICENSE](LICENSE).
