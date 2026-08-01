# Cover art, and why this app is written in C

Status: **settled.** Piu on Pebble cannot display an image that arrives at
runtime, so cover art is impossible under Alloy at any size. That is the reason
the app moved from the `alloy` branch to C.

Everything below was read from the SDK sources in
`~/.local/share/pebble-sdk/SDKs/4.17/toolchain/moddable` or measured on a
physical Pebble Time 2 against LMS 9.1.1.

## The format and what it costs

Pebble textures are a pair: an optional `Gray4` alpha mask and an `ARGB2222`
colour bitmap. `ARGB2222` maps to `GBitmapFormat8Bit` with
`row_size_bytes = width`, so an image costs exactly `width * height` bytes — one
byte per pixel, two bits per channel, `0xC0` for full alpha.

| edge | bytes |
| --- | --- |
| 100 x 100 | 10000 |
| 120 x 120 | 14400 |
| 160 x 160 | 25600 |
| 200 x 166 | 33200 |
| 260 x 260 | 67600 |

## Why Alloy cannot do it

Four independent doors, all closed.

**1. `piuTexture.c` mistakes an offset for a pointer.**

`modules/piu/Pebble/piuTexture.c` accepts a Commodetto Bitmap and keeps its
pixel address:

```c
self->bits.addr = cb->bits.data;
```

It never consults `cb->havePointer`. But `commodettoBitmap.c` sets that field
precisely because the answer varies:

```c
if (xsBufferRelocatable == xsmcGetBufferReadable(xsArg(3), (void**)&data, &dataSize)) {
    cb.havePointer = false;
    cb.bits.offset = offset;              // an OFFSET: this buffer moves
} else {
    cb.havePointer = true;
    cb.bits.data = offset + (char*)data;  // a real pointer: this one does not
}
```

Both write into the same union. Hand Piu a plain `ArrayBuffer` and the texture
paints from an offset interpreted as an address — garbage from the first frame,
no garbage collection required. An earlier note in this file guessed that heap
compaction invalidated a valid pointer; the truth is that there never was one.

**2. Nothing in JS produces a non-relocatable buffer at runtime.**

`xsmc.c` is explicit: only `XS_HOST_KIND` with a buffer-info slot — that is,
memory handed over by `xsSetHostBuffer` — counts as non-relocatable. Plain
`ArrayBuffer`s and typed arrays over them are relocatable. Of the modules in the
Pebble host, only `Resource` uses `xsSetHostBuffer`, and that reads flash: build
time, not run time.

**3. Custom native bindings cannot supply one either.**

`ModdableCreationRecord` has an `fxBuildFFI` hook and `xsffi.h` ships in the
app-visible include path, so adding C functions to the machine is officially
supported. The exported `txAPI` table, however, offers only `fromArrayBuffer`
and `toArrayBufferHandle` — both relocatable, and the handle type says so. There
is no host-buffer call.

**4. Poco is not a way around Piu.**

`PocoBitmapDraw` in `commodettoPocoBlit-pebble.c` accepts `Pebble`,
`MonochromeAligned` and `Gray4`, and asserts on anything else. ARGB2222 cannot
be drawn through Poco at all. `Bitmap.Pebble` wraps a real `GBitmap`, but the
only thing that builds one is `xs_pebblebitmap_build`, from a resource id.

For completeness: the JPEG and PNG decoders are not in the Pebble host manifest,
and a mod contains JS bytecode only, so it cannot bring its own native code.

## Baking the image in does not work either

The obvious fallback is to ship the pixels as a resource. Measured on hardware
with a 10 KB test image added to the mod's `data` section:

| | before | with a 10 KB image |
| --- | --- | --- |
| `mc.xsa` | 17562 B | 28264 B |
| app heap used | 111312 B | 121276 of 122568 B |
| second launch | fine | `fxMapArchive failed` → `fxAbort` |

`ArchivePebbleResource.c` obtains the archive through
`applib_resource_mmap_or_load`, and it lands in app RAM in full. Under Alloy an
image costs its own size in the app heap whether it is baked in or not — on top
of the 86 KB XS machine. Ten kilobytes was enough to make the app unlaunchable.

## What C gives instead

`gbitmap_create_blank()` returns a buffer the app owns, at a fixed address, and
chunks are copied into `gbitmap_get_data()`. Nothing moves and nothing is
reinterpreted.

Measured free app heap at startup, same watch, same app:

| | free heap |
| --- | --- |
| Alloy | 12140 B |
| C | ~123000 B |

A 200x166 cover needs 33200 of that.

## The transport, when we build it

`https://github.com/alex523ap/Playback-for-Spotify` solves the same problem in C
on the same two platforms, and its player screen is the design this app is
aiming at. Worth copying from `src/pkjs/image_transfer.js`:

- `jpeg-js`, a pure-JS decoder, bundled into the phone side — PebbleKit JS has
  no image decoding and no canvas
- quantisation to ARGB2222 is one line:
  `dst[i] = 0xC0 | ((r >> 6) << 4) | ((g >> 6) << 2) | (b >> 6)`
- chunks of 2000 bytes on emery and gabbro
- sizes: emery 200x166, gabbro 260x260, plus a second buffer of the same size to
  prefetch the next track's art
- LMS can resize server-side via `/music/<id>/cover_100x100_o.jpg`, which saves
  the decoder most of its work

## Worth reporting upstream

The mismatch in point 1 is a contained bug: `piuTexture.c` would have to honour
`cb->havePointer` and resolve the address per draw rather than once at
construction. Fixing it would let every Alloy app show a runtime image. A report
to Rebble with this analysis costs nothing and would not change this app's
direction either way.
