# Upstream bug report: pebbleproxy header parsing

Prepared text for an issue against Moddable. This is the reason this project
routes network traffic through the phone instead of using on-device `fetch()`
— see [../networking.md](../networking.md) for the wider picture.

**Status:** not filed yet. Update this line with the issue URL once it is.

---

**Title:** `@moddable/pebbleproxy` 0.1.8: header parsing breaks every request with headers (`setRequestHeader('')`)

**Repository:** Moddable-OpenSource/moddable — `build/devices/pebble/npm/pebbleproxy`

---

### Summary

With `@moddable/pebbleproxy` 0.1.8 and Pebble SDK 4.17, every HTTP request made
from the watch fails on the phone side. Both `fetch()` and the ECMA-419
`device.network.http.io` client are affected, because both go through the same
proxy. The proxy logs:

```
moddable proxy http exception
SyntaxError: Failed to execute 'setRequestHeader' on 'XMLHttpRequest':
'' is not a valid HTTP header field name.
```

### Environment

- Pebble Time 2 (emery), PebbleOS via the official companion app on Android
- pebble-tool 5.0.39, SDK 4.17
- `@moddable/pebbleproxy` 0.1.8 (current on npm as of 2026-02-17)
- pkjs wired exactly as in the official `hellofetch` example

### Root cause

`proxy.js`, in the `makeRequest` case:

```js
request.headers.split("\n").forEach(line => {
    const [key, value] = line.split(":");
    request.xhr.setRequestHeader(key, value);
});
```

The HTTP client in SDK 4.17 sends each request header as `name:value\n`,
including a trailing `\n` after the last header
(`build/devices/pebble/modules/httpclient/httpclient-pebble.js`, `sendHeaders`
state):

```js
this.#remain = ArrayBuffer.fromString(`${item.value[0]}:${item.value[1]}\n`);
```

So `request.headers` ends with a newline, `split("\n")` produces an empty final
element, and that element yields `key === ""` and `value === undefined` →
`setRequestHeader("", undefined)` throws. Any request that sets at least one
header hits this, which in practice means all of them.

There is a second bug in the same two lines: `line.split(":")` destructures only
the first two segments, so any header **value** containing a colon is silently
truncated at the first one. A `date` header
(`Thu, 01 Jan 1970 00:00:00 GMT`) becomes `Thu, 01 Jan 1970 00`. This affects
the official `hellohttpclient` example, which sends exactly that header.

### Reproduce

1. `pebble new-project --alloy demo`
2. `pebble package install @moddable/pebbleproxy`
3. Wire `src/pkjs/index.js` as in `hellofetch`
4. In `src/embeddedjs/main.js`, issue any request with a header, e.g.

```js
const client = new device.network.http.io({
    ...device.network.http, host: "example.com", port: 80
});
client.request({
    method: "GET",
    path: "/",
    headers: new Map([["content-type", "text/plain"]]),
    onHeaders(status) { console.log("status " + status); },
    onReadable(count) { this.read(count); },
    onDone() { console.log("done"); }
});
```

5. `pebble install --phone <ip> --logs`

`onHeaders` never fires; the log shows the `setRequestHeader` SyntaxError.

### Suggested fix

```diff
-			request.headers.split("\n").forEach(line => {
-				const [key, value] = line.split(":");
-				request.xhr.setRequestHeader(key, value);
-			});
+			request.headers.split("\n").forEach(line => {
+				const idx = line.indexOf(":");
+				if (idx <= 0) return;   // skip empty lines, e.g. the trailing newline
+				request.xhr.setRequestHeader(line.slice(0, idx), line.slice(idx + 1));
+			});
```

This fixes both problems: empty lines are skipped, and only the first colon is
treated as the separator so values keep theirs.

### Verified

With that patch applied to `node_modules/@moddable/pebbleproxy/proxy.js` (and a
`pebble clean` — the build does not pick up `node_modules` changes otherwise),
the `setRequestHeader` error disappears and responses arrive on the watch.

A separate follow-up problem appears after that, which I have not narrowed down:
the app then terminates with `Alloy fatal error, memory full` on a 387-byte
response. Happy to open that separately if useful.

### Side note on the emulator

In the QEMU emulator (`pypkjs`), the same unpatched setup produces a different
symptom: the request completes, but the wrong one is issued — a `GET /` instead
of the requested `POST /jsonrpc.js`, confirmed byte-exactly by response size.
That is consistent with the malformed request line derailing the proxy's parse
before the header loop, so it may be the same root cause.
