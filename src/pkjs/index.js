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

var cachedMenu = {}; // maps itemId -> full LMS item object


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

  if (op === "menu") {
    var parts = String(arg).split(FIELD);
    var player = parts[0];
    var start = parseInt(parts[1], 10) || 0;
    var limit = parseInt(parts[2], 10) || 10;
    var reqType = parts[3] || "node"; // "node" or "cmd"
    var reqId = parts[4] || "home";
    
    if (reqType === "node") {
        rpc(player, ["menu", 0, 999, "direct:1"], function(err, json) {
            var loop = json && json.result ? (json.result.item_loop || json.result.loop_loop || json.result.playlist_loop) : null;
            if (err || !loop) {
                reply(id, err || "no items", "");
                return;
            }
            loop = loop.filter(function(i) { return i.node === reqId; });
            var out = [];
            for (var i = start; i < start + limit && i < loop.length; i++) {
                var item = loop[i];
                var itemId = item.id || ("_" + reqId + "_" + i);
                cachedMenu[itemId] = item;
                
                var folderType = "0";
                if (item.isANode) folderType = "node";
                else if ((item.actions && item.actions.go) || item.type === "playlist" || item.type === "album") folderType = "cmd";
                var text = (item.text || item.name || item.title || "?").substring(0, 30);
                out.push([text, itemId, folderType].join(FIELD));
            }
            reply(id, "", out.join(RECORD));
        });
    } else if (reqType === "cmd") {
        var parentItem = cachedMenu[reqId];
        if (!parentItem) {
            reply(id, "parent not found", "");
            return;
        }
        var req = [];
        if (parentItem.actions && parentItem.actions.go) {
            req = parentItem.actions.go.cmd.slice();
            req.push(start, limit);
            if (parentItem.actions.go.params) {
                for (var key in parentItem.actions.go.params) {
                    req.push(key + ":" + parentItem.actions.go.params[key]);
                }
            }
        }
        if (!req.length) {
            reply(id, "no cmd", "");
            return;
        }
        rpc(player, req, function(err, json) {
            var loop = json && json.result ? (json.result.item_loop || json.result.loop_loop || json.result.playlist_loop) : null;
            if (err || !loop) {
                reply(id, err || "no items", "");
                return;
            }
            // already extracted
            var out = [];
            // Force slice to prevent huge memory allocations if plugin ignores start/limit
            var sliceStart = loop.length > limit ? start : 0;
            for (var i = sliceStart; i < sliceStart + limit && i < loop.length; i++) {
                var item = loop[i];
                var itemId = item.id || ("_" + reqId + "_" + i);
                cachedMenu[itemId] = item;
                
                var folderType = "0";
                if (item.isANode) folderType = "node";
                else if ((item.actions && item.actions.go) || item.type === "playlist" || item.type === "album") folderType = "cmd";
                var text = (item.text || item.name || item.title || "?").substring(0, 30);
                out.push([text, itemId, folderType].join(FIELD));
            }
            reply(id, "", out.join(RECORD));
        });
    }
    return;
  }

  if (op === "menu_go") {
    var parts = String(arg).split(FIELD);
    var player = parts[0];
    var itemId = parts[1];
    var item = cachedMenu[itemId];
    
    if (!item) {
        reply(id, "item not found", "");
        return;
    }
    
    var req = null;
    if (item.actions && item.actions.go) {
       req = item.actions.go.cmd.slice();
       if (item.actions.go.params) {
           for (var key in item.actions.go.params) {
               req.push(key + ":" + item.actions.go.params[key]);
           }
       }
    } else if (item.actions && item.actions.do) {
       req = item.actions.do.cmd.slice();
       if (item.actions.do.params) {
           for (var key in item.actions.do.params) {
               req.push(key + ":" + item.actions.do.params[key]);
           }
       }
    }
    
    if (req) {
        rpc(player, req, function (err, json) {
            reply(id, err, "ok");
        });
    } else {
        reply(id, "no action", "");
    }
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
