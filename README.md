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
    lms.js           LMS interface (players, status, command)
    relay.js         Transport to the phone side over AppMessage
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

No configuration page yet. Until then the server address, port and credentials
live in `DEFAULTS` in `src/pkjs/index.js`, overridable via
`localStorage["lms"]` on the phone.

## Status

Working: player list from the server, Up/Down selection, status view with
artist, title, volume and playback state, Back to return to the list.

Not yet done: playback control (`lms.command()` exists but is not bound to
buttons), volume, playlist and music folder navigation, configuration page,
AppGlance, voice search, chunking for long responses.

## History

Originally by Christian Herzog (daduke). The Pebble.js version on `master`
dates from 2016 and ran unchanged for ten years.

## License

See [LICENSE](LICENSE).
