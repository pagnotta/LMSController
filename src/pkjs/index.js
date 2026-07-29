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

var RECORD = "\u001e";
var FIELD = "\u001f";

// AppMessage payloads are small. Responses are hard-capped; longer lists will
// need a chunking extension to the protocol.
var MAX_DATA = 512;

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

function handle(id, op, arg) {
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

// Configuration page not implemented yet. Until then DEFAULTS above and
// localStorage["lms"] on the phone apply.
Pebble.addEventListener("showConfiguration", function () {
  console.log("no configuration page yet, current server: " + baseUrl());
});
