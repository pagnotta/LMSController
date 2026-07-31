/**
 * Phone-side end of the relay.
 *
 * All HTTP traffic to LMS happens here. The watch only ever receives compact,
 * field-separated strings -- no JSON, no large bodies. That avoids both the
 * header bug in @moddable/pebbleproxy and the memory limits of the mod
 * (see docs/networking.md).
 *
 * Protocol: see src/embeddedjs/relay.js
 */

var Clay = require('@rebble/clay');
var clayConfig = require('./config.json');
var customClay = new Clay(clayConfig, null, { autoHandleEvents: false });


var RECORD = "\u001e";
var FIELD = "\u001f";

// AppMessage payloads are small. Responses are hard-capped; longer lists will
// need a chunking extension to the protocol.
var MAX_DATA = 512;

// Maps itemId -> { item: <LMS item>, base: <the response's base object> }.
// The base has to be kept: LMS puts the actions for a whole list there, not
// into the items (see resolveAction).
var cachedMenu = {};


var DEFAULTS = {
  protocol: "http",
  host: "192.168.178.85",
  port: "9000",
  user: "",
  password: ""
};

function loadSettings() {
  try {
    var raw = localStorage.getItem("lms");
    if (raw) {
      var s = JSON.parse(raw);
      return {
        protocol: s.protocol || DEFAULTS.protocol,
        host: s.host || DEFAULTS.host,
        port: String(s.port || DEFAULTS.port),
        user: s.user || "",
        password: s.password || ""
      };
    }
  } catch (e) {
    console.log("settings unreadable, using defaults: " + e);
  }
  return DEFAULTS;
}

var settings = loadSettings();

function baseUrl() {
  return settings.protocol + "://" + settings.host + ":" + settings.port + "/jsonrpc.js";
}

/** JSON-RPC call to LMS. player may be "-" for server-wide commands. */
function rpc(player, params, done) {
  var xhr = new XMLHttpRequest();
  xhr.open("POST", baseUrl(), true);
  xhr.timeout = 8000;
  xhr.setRequestHeader("Content-Type", "application/json");

  if (settings.password && typeof btoa === "function") {
    xhr.setRequestHeader(
      "Authorization",
      "Basic " + btoa(settings.user + ":" + settings.password)
    );
  }

  xhr.onload = function () {
    if (xhr.status < 200 || xhr.status > 299) {
      done("HTTP " + xhr.status);
      return;
    }
    // done() is called OUTSIDE the try on purpose: otherwise the catch swallows
    // every downstream error and misreports it as a parse failure.
    var json;
    try {
      json = JSON.parse(xhr.responseText);
    } catch (e) {
      done("response is not JSON");
      return;
    }
    done(null, json);
  };
  xhr.onerror = function () { done("network error"); };
  xhr.ontimeout = function () { done("timeout"); };

  xhr.send(JSON.stringify({ id: 1, method: "slim.request", params: [player, params] }));
}

/** name<FIELD>id<FIELD>playing, records separated by RECORD. */
function encodePlayers(json) {
  var loop = (json.result && json.result.players_loop) || [];
  var out = [];
  for (var i = 0; i < loop.length; i++) {
    var p = loop[i];
    out.push([p.name || "?", p.playerid || "", p.isplaying ? "1" : "0"].join(FIELD));
  }
  return out.join(RECORD);
}

/** artist<FIELD>title<FIELD>volume<FIELD>playing */
function encodeStatus(json) {
  var r = json.result || {};
  var track = (r.playlist_loop && r.playlist_loop[0]) || {};
  return [
    track.artist || r.artist || "",
    track.title || r.title || r.current_title || "",
    String(r["mixer volume"] || 0),
    r.mode === "play" ? "1" : "0"
  ].join(FIELD);
}

function reply(id, err, data) {
  var payload = { RS_ID: id, RS_ERR: err ? String(err) : "" };
  payload.RS_DATA = data ? String(data).substring(0, MAX_DATA) : "";
  Pebble.sendAppMessage(payload, function () {}, function (e) {
    console.log("sendAppMessage failed: " + JSON.stringify(e));
  });
}

/**
 * Finds an action for an item.
 *
 * LMS sends the actions for a whole list once, in the response's `base`, and
 * leaves the items carrying only their own parameters. An album, for instance,
 * arrives as `{text, type, commonParams: {album_id: 7958}}` with no `actions`
 * at all, while `base.actions.go` holds the command and names `commonParams`
 * via its `itemsParams` field. Looking only at `item.actions` therefore finds
 * nothing for albums, tracks, and most plugin menus -- which is why selecting
 * them used to do nothing.
 */
function resolveAction(name, item, base) {
  if (item.actions && item.actions[name])
    return item.actions[name];
  if (base && base.actions && base.actions[name])
    return base.actions[name];
  return null;
}

/** Assembles the slim.request words: cmd, paging, action params, item params. */
function buildRequest(action, item, start, limit) {
  var req = action.cmd.slice();
  var key;
  if (start !== undefined)
    req.push(start, limit);
  if (action.params)
    for (key in action.params)
      req.push(key + ":" + action.params[key]);
  var own = action.itemsParams && item[action.itemsParams];
  if (own)
    for (key in own)
      req.push(key + ":" + own[key]);
  return req;
}

/**
 * What selecting this item should do.
 *
 * `go` is always the action behind Select; whether it browses or plays is what
 * `nextWindow` tells us. On the album level `base.actions.go` runs
 * `browselibrary items` with no nextWindow, so it opens a list. One level down,
 * on tracks, `go` runs `playlistcontrol` with `nextWindow: "nowPlaying"` -- the
 * same key, but it starts playback. So there is nothing to guess from types.
 *
 *   "node"  a section of the home menu, filtered on this side
 *   "cmd"   go returns a new list -- browse into it
 *   "play"  go (or do) starts playback
 *   "0"     nothing to do
 */
function classify(item, base) {
  if (item.isANode)
    return "node";
  var go = resolveAction("go", item, base);
  if (go)
    return go.nextWindow ? "play" : "cmd";
  if (resolveAction("play", item, base) || resolveAction("do", item, base))
    return "play";
  return "0";
}

/** One line per item, cut on a record boundary so no half record is sent. */
function encodeItems(loop, base, reqId, start, limit, total) {
  var out = String(total);
  var budget = MAX_DATA;
  for (var i = start; i < start + limit && i < loop.length; i++) {
    var item = loop[i];
    var itemId = item.id || ("_" + reqId + "_" + i);
    cachedMenu[itemId] = { item: item, base: base };

    // Album titles arrive as "album\nartist"; the watch shows a single line.
    var text = String(item.text || item.name || item.title || "?")
      .replace(/\s*\n\s*/g, " - ").substring(0, 28);
    var record = RECORD + [text, itemId, classify(item, base)].join(FIELD);

    // Truncating mid-record used to drop the last item silently, which made a
    // full page look short and the list appear to end early.
    if (out.length + record.length > budget)
      break;
    out += record;
  }
  return out;
}

function handle(id, op, arg) {
  // Diagnostics from the watch. src/embeddedjs/ has no route to `pebble logs`
  // of its own, so it borrows the relay. See src/embeddedjs/diag.js.
  if (op === "log") {
    console.log("watch: " + arg);
    reply(id, null, "ok");
    return;
  }

  if (op === "players") {
    rpc("-", ["serverstatus", 0, 99], function (err, json) {
      reply(id, err, err ? "" : encodePlayers(json));
    });
    return;
  }

  if (op === "status") {
    rpc(arg, ["status", "-", 1, "tags:al"], function (err, json) {
      reply(id, err, err ? "" : encodeStatus(json));
    });
    return;
  }

  if (op === "cmd") {
    var parts = String(arg).split(FIELD);
    var player = parts[0];
    var words = (parts[1] || "").split(" ").filter(function (w) { return w.length; });
    if (!words.length) {
      reply(id, "empty command", "");
      return;
    }
    rpc(player, words, function (err) { reply(id, err, "ok"); });
    return;
  }

  if (op === "menu") {
    var parts = String(arg).split(FIELD);
    var player = parts[0];
    var start = parseInt(parts[1], 10) || 0;
    var limit = parseInt(parts[2], 10) || 10;
    var reqType = parts[3] || "node"; // "node" or "cmd"
    var reqId = parts[4] || "home";
    
    if (reqType === "node") {
        // The home menu arrives whole and is split into sections by `node`, so
        // paging happens here rather than on the server.
        rpc(player, ["menu", 0, 999, "direct:1"], function(err, json) {
            var loop = json && json.result ? json.result.item_loop : null;
            if (err || !loop) {
                reply(id, err || "no items", "");
                return;
            }
            loop = loop.filter(function(i) { return i.node === reqId; });
            reply(id, "", encodeItems(loop, json.result.base, reqId, start, limit, loop.length));
        });
    } else if (reqType === "cmd") {
        var parent = cachedMenu[reqId];
        if (!parent) {
            reply(id, "parent not found", "");
            return;
        }
        var action = resolveAction("go", parent.item, parent.base);
        if (!action) {
            reply(id, "no cmd", "");
            return;
        }
        rpc(player, buildRequest(action, parent.item, start, limit), function(err, json) {
            var result = json && json.result;
            var loop = result ? (result.item_loop || result.loop_loop || result.playlist_loop) : null;
            if (err || !loop) {
                reply(id, err || "no items", "");
                return;
            }
            // LMS honours start/limit, so what came back is already the page and
            // is indexed from 0. A plugin that ignores paging returns everything,
            // and then the page has to be cut out here.
            var sliceStart = loop.length > limit ? start : 0;
            // `count` is the total for the level; without it the caller cannot
            // tell the end of the list from a page that was cut for size.
            var total = (result.count === undefined) ? (sliceStart + loop.length) : parseInt(result.count, 10);
            reply(id, "", encodeItems(loop, result.base, reqId, sliceStart, limit, total));
        });
    }
    return;
  }

  if (op === "menu_go") {
    var parts = String(arg).split(FIELD);
    var player = parts[0];
    var itemId = parts[1];
    var entry = cachedMenu[itemId];

    if (!entry) {
        reply(id, "item not found", "");
        return;
    }

    // Same order the watch's classify() used: whatever "go" resolves to is the
    // action behind Select, and for a track that is playlistcontrol.
    var goAction = resolveAction("go", entry.item, entry.base) ||
                   resolveAction("play", entry.item, entry.base) ||
                   resolveAction("do", entry.item, entry.base);
    if (!goAction) {
        reply(id, "no action", "");
        return;
    }

    // No start/limit here: this runs the action, it does not page a list.
    rpc(player, buildRequest(goAction, entry.item), function (err) {
        reply(id, err, "ok");
    });
    return;
  }

  reply(id, "unknown operation: " + op, "");
}

Pebble.addEventListener("ready", function () {
  console.log("LMS relay ready, server: " + baseUrl());

  // Handshake: the watch cannot send until the AppMessage session exists.
  // Without this first message from the phone, the channel on the watch stays
  // "not writable" forever and onWritable never fires.
  // RS_ID 0 has no waiter on the watch and is discarded there.
  reply(0, null, "ready");
});

Pebble.addEventListener("appmessage", function (e) {
  var p = e.payload || {};
  if (p.RQ_ID === undefined)
    return;
  handle(p.RQ_ID, p.RQ_OP, p.RQ_ARG || "");
});

// Configuration page via Clay
Pebble.addEventListener('showConfiguration', function(e) {
  Pebble.openURL(customClay.generateUrl());
});

Pebble.addEventListener('webviewclosed', function(e) {
  if (e && !e.response) {
    return;
  }
  var dict = customClay.getSettings(e.response, false);
  
  // Save settings to localStorage
  settings.protocol = dict.protocol.value;
  settings.host = dict.host.value;
  settings.port = dict.port.value;
  settings.user = dict.user.value;
  settings.password = dict.password.value;
  
  localStorage.setItem("lms", JSON.stringify(settings));
  console.log("Settings updated. New server: " + baseUrl());
});
