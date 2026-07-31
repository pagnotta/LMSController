# LMSController

Control your Logitech Media Server players from your Pebble.

This branch (`alloy`) is the rewrite on the **Alloy SDK** — JavaScript running
natively on the watch, with the Piu UI framework. Target platforms are
**emery** (Pebble Time 2) and **gabbro** (Pebble Round 2).

The original Pebble.js version is untouched on `master`. It still runs, but it
no longer builds with a current SDK and does not support the new platforms.

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
  embeddedjs/        JavaScript on the watch (Moddable XS)
    main.js          Piu UI: player list, status view
    theme.js         Skins, styles and metrics for emery and gabbro
    lms.js           LMS interface (players, status, command, menu)
    relay.js         Transport to the phone side over AppMessage
    diag.js          Memory diagnostics, see "Memory" below
    manifest.json    Moddable mod manifest
  pkjs/
    index.js         HTTP to LMS, reduces responses for the watch
  c/
    mdbl.c           C entry point, boots the JS machine
```

Networking deliberately goes through the phone rather than on-device
`fetch()`. The reasons and measurements are in
[docs/networking.md](docs/networking.md).

## Configuration

Server address, port and credentials are set on the phone through the Clay
configuration page. `DEFAULTS` in `src/pkjs/index.js` applies until then.

## Memory

The XS machine Alloy creates by default is small: 8 KB chunk heap, 512 slots
and 384 stack slots, and it cannot grow. `src/c/mdbl.c` asks for a larger one
through `ModdableCreationRecord`, which brings it to 48 KB of chunks and 32 KB
of slots — measured, the app peaks near both limits while browsing.

**The QEMU emulator ignores that record.** An allocation probe held 40 KB on a
physical watch and 1 KB in the emulator, and chunk sizes of 1 KB and 900 KB
behave there exactly like the default. Memory behaviour can only be judged on
real hardware. `src/embeddedjs/diag.js` carries the probe and an optional
on-screen counter overlay; on a watch the firmware also logs an `instruments:`
line every second, which is usually the easier read.

Two consequences for anything added here. Slots, not chunks, are the scarce
resource, so promises are avoided in favour of callbacks. And an unhandled
promise rejection is fatal under XS — `fxAbort unhandled rejection`, the app
disappears — which is the second reason the transport is callback-based.

## Status

Working: player list, status view with artist, title, volume and playback
state, touch gestures for play/pause, track and volume, browsing the full LMS
menu tree with paging, and starting playback from it. Back steps one level up.

Not yet done: cover art, AppGlance, voice search, volume on the status view
buttons. The player list still renders every player without a scroll window, so
more players than fit on screen would be clipped.

## History

Originally by Christian Herzog (daduke). The Pebble.js version on `master`
dates from 2016 and ran unchanged for ten years.

## License

See [LICENSE](LICENSE).
