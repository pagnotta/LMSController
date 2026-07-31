# Cover art under Alloy: what is known so far

Status: **investigation paused, no decision taken.** Everything below was
measured on a physical Pebble Time 2 (emery) against LMS 9.1.1, except where
marked as inference.

## The display path works

Piu can show an image built at runtime — it is simply not documented. The
guide at developer.repebble.com only shows `new Texture("logo.png")` and
`new Texture(2)`, both of which read a resource baked into the app. The
constructor in `modules/piu/Pebble/piuPebble.js` takes more than that:

```js
constructor(it, alphaBitmap, colorBitmap) {
    if (alphaBitmap || colorBitmap) {
        this._create(alphaBitmap, colorBitmap);
        return;
    }
```

So a Commodetto Bitmap goes in as the **second or third** argument. Watch the
positions: the colour bitmap belongs in the third. Passing it second makes it
the alpha mask, and that branch of `piuTexture.c` accepts only
`MonochromeAligned` or `Gray4`, failing with "invalid texture format" — which
is exactly what a first attempt produced.

A Pebble texture is a pair: a `Gray4` alpha mask and an `ARGB2222` colour
bitmap. Resources are split into `<name>-alpha.bm4` and `<name>-color.bm4` at
build time. The mask is optional, and cover art does not need one.

## The format and what it costs

`ARGB2222` maps to `GBitmapFormat8Bit` with `row_size_bytes = width`, so an
image costs exactly `width * height` bytes — one byte per pixel, two bits per
channel, `0xC0` for full alpha.

| edge | bytes |
| --- | --- |
| 100 x 100 | 10000 |
| 120 x 120 | 14400 |
| 140 x 140 | 19600 |
| 160 x 160 | 25600 |
| 200 x 166 | 33200 |
| 260 x 260 | 67600 |

Against that: the chunk heap is 48 KB (see `src/c/mdbl.c`), about 11 KB of it
already in use once the UI is up, and it peaks near the top while browsing
menus. The app heap as a whole reports 111312 of 122568 bytes used, so the
machine cannot simply be made bigger — more chunk memory has to come out of
the 32 KB slot budget.

## What a working C app does

`https://github.com/alex523ap/Playback-for-Spotify` solves the same problem in
C, and its player screen is the design this port is aiming at: full-width cover
on top, title, artist, progress bar.

Its numbers, from `src/pkjs/image_transfer.js`:

- emery 200 x 166, gabbro 260 x 260, chalk 180 x 180, basalt 144 x 106,
  aplite 96 x 96
- on emery and gabbro it also keeps a **second** buffer of the same size, to
  prefetch the next track's art
- chunks of 2000 bytes on basalt, emery and gabbro; 1000 elsewhere

Two of those are out of reach here: gabbro's 67600 bytes exceed the whole chunk
heap, and a prefetch buffer doubles a cost that is already the binding
constraint. emery's 33200 might fit if memory is shifted from slots to chunks,
but it would consume nearly all of the remaining budget.

The transport, on the other hand, carries over almost unchanged and is worth
copying:

- `jpeg-js`, a pure-JS decoder, bundled into `src/pkjs/` — PebbleKit JS has no
  image decoding of its own, no canvas, and the Alloy host ships `parseBMP` but
  no JPEG decoder, so this has to happen on the phone
- quantisation to ARGB2222 is one line:
  `dst[i] = 0xC0 | ((r >> 6) << 4) | ((g >> 6) << 2) | (b >> 6)`
- LMS can resize server-side via `/music/<id>/cover_100x100_o.jpg`, which saves
  the decoder most of its work

C also sidesteps a problem Alloy has. There, `gbitmap_create_blank()` gives the
app a buffer it owns, and chunks are copied into `gbitmap_get_data()`. It never
moves.

## The open question

Under Alloy the pixels live in the XS chunk heap, which the collector
**compacts**, while `piuTexture.c` keeps a raw pointer into it:

```c
self->bits.addr = cb->bits.data;
```

If a collection moves the buffer, the texture points at nothing. That would
make cover art impossible under Alloy at any size, so it has to be settled
before anything else is built.

This is inference from the source, not a measurement. A 100 x 100 test image
did come out garbled on the watch, which fits — but two mistakes in the test
harness fit just as well, and they have to be ruled out first:

- the test Content is added to the Column, which squeezes it into whatever
  vertical space is left over and clips the image
- `fill()` walks every child and hides the ones past the last text line, so the
  cover disappears as soon as the status view is redrawn

`src/embeddedjs/diag.js` carries the test behind `coverTestSize`, currently 0.
Setting it to an edge length draws a four-band colour ramp in the status view,
coarse enough that a wrong stride or format is obvious rather than subtle.

A clean experiment would place the image outside the Column, leave it up while
navigating, and watch the `instruments:` line for a garbage collection —
whether it survives one is the whole answer.
