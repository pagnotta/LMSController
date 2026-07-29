# Networking under Alloy: why a relay instead of `fetch()`

Alloy apps have two JavaScript environments: `src/embeddedjs/` runs on the
watch, `src/pkjs/` on the phone. The watch has no IP stack of its own, so every
HTTP request has to travel through the phone.

There are two ways to do that. This project uses the second one.

## Option 1: on-device `fetch()` / `httpclient` via `@moddable/pebbleproxy`

The officially intended route. `fetch()`, `WebSocket` and the ECMA-419
`device.network.http.io` all exist in the Alloy host, but they require the npm
package `@moddable/pebbleproxy`, which acts as a bridge on the pkjs side.

**Tested on a Pebble Time 2 (emery) with SDK 4.17 and pebbleproxy 0.1.8 — it
does not work.** Every request fails with:

```
moddable proxy http exception
SyntaxError: Failed to execute 'setRequestHeader' on 'XMLHttpRequest':
'' is not a valid HTTP header field name.
```

The cause is in `proxy.js`:

```js
request.headers.split("\n").forEach(line => {
    const [key, value] = line.split(":");
    request.xhr.setRequestHeader(key, value);
});
```

The client in SDK 4.17 sends headers as `name:value\n`, including a trailing
`\n` after the last one. `split("\n")` therefore yields an empty final entry,
which becomes `setRequestHeader("", undefined)`. The same line has a second
bug: `line.split(":")` truncates any header value that itself contains a colon
(a date, for instance) — which affects even the official `hellohttpclient`
example with its `date` header.

Patching this locally (`indexOf(":")` instead of `split(":")`, skip empty
lines) gets past the header error. The response then arrived, but the app died
with `Alloy fatal error, memory full`. That was not investigated further.

Other observations about this route:

- Without the proxy, `fetch()` hangs silently — no error, no response.
- With the proxy, `fetch()` dies while receiving the body, before JavaScript
  ever sees the bytes. `String.fromArrayBuffer` and `JSON.parse` are not
  involved.
- `httpclient` survives where `fetch()` dies, but in the QEMU emulator it
  fetched `GET /` instead of `POST /jsonrpc.js` (proven byte-exactly by
  response size), because the malformed request line confuses the proxy parser.
- In the emulator, `pypkjs` does not proxy TCP for Alloy at all, so testing
  this route requires real hardware.

If Moddable fixes this, switching over is small: replace `relay.js` only —
`lms.js` and the UI stay as they are.

## Option 2: relay over AppMessage (used here)

HTTP happens in `src/pkjs/index.js` using `XMLHttpRequest`, and the watch
receives only compact, field-separated strings. No `@moddable/pebbleproxy`, no
body streaming across the bridge, and the memory pressure stays on the phone.
This is also how the old Pebble.js version (branch `master`) worked.

Verified in the emery emulator against LMS 9.1.1: both the player list and the
status view arrive with real data.

### Three pitfalls that cost time

**`Message` must be constructed at module scope.** Created inside a Piu display
callback (`onDisplaying`), `onWritable` never fires and the app exits without
any error message. The official `hellomessage` example also constructs
`Message` at module scope.

**Set `input`/`output` explicitly.** Without those options `Message` opens the
channel with maximum buffers, visible in the log as:

```
app_message_open() called with app_message_outbox_size_maximum().
This consumes 8200 bytes of heap memory, potentially more in the future!
```

8200 bytes in plus 8200 out is 16 KB taken away from the XS mod, which leads to
`memory full`. This protocol needs a fraction of that.

**The phone has to send first.** Until an AppMessage arrives from the phone,
the channel on the watch stays permanently `not writable`, and `onWritable`
does not fire on its own — the watch cannot get out of that state by itself.
That is why `src/pkjs/index.js` sends a handshake from its `ready` handler
(`RS_ID` 0, discarded on the watch). On top of that, `relay.js` writes directly
and retries on failure instead of waiting for `onWritable`.

For diagnosis, `relay.debug()` returns the transport state as a short string:
`w<onWritable events> s<writes> r<reads> q<queue length>`. So `w0 s0 r0 q1`
means: a request is queued and the channel never became writable.

## Open issues

- Responses are capped at `MAX_DATA` (512 bytes). Longer lists — playlists or
  music folders — need chunking in the protocol.
- The configuration page is missing; until then `DEFAULTS` in
  `src/pkjs/index.js` and `localStorage["lms"]` on the phone apply.

## Tooling notes

`console.log` from `src/embeddedjs/` does **not** show up in `pebble logs` —
only the pkjs side is logged. For the watch side there are two options:

- render the state into the UI (Piu or Poco), or
- store markers in `localStorage` and display them on the next launch, which
  survives a crash.

For real JS debugging, `pebble build --debug` plus xsbug is the intended path
(`toolchain/moddable-tools/xsbug`).

After an app crash the emulator becomes unresponsive: `pebble screenshot` and
`pebble ping` time out. Use `pebble kill --force` and reinstall.
