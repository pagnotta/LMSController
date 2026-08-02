/**
 * Just enough PNG to read cover art, in ES5.
 *
 * Needed because LMS will not always give us a JPEG. Its image proxy keeps the
 * source's transparency, and a station logo with an alpha channel -- TSF Jazz
 * ships a PWA icon -- comes back as PNG no matter which resize mode or
 * background colour is asked for, and regardless of the .jpg in the URL.
 *
 * Scope is deliberately narrow: 8 bits per channel, no interlace, which is what
 * LMS emits. Anything else throws rather than returning something plausible and
 * wrong. Output matches jpeg-js -- {width, height, data} with RGBA bytes -- so
 * the caller does not care which decoder ran.
 *
 * Only inflate is pulled in from pako; the deflate half would double the
 * bundle for nothing.
 */

var inflate = require('pako/lib/inflate');

var SIGNATURE = [0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A];

/** Bytes per pixel for the colour types this handles, at 8 bits per channel. */
var CHANNELS = { 0: 1, 2: 3, 3: 1, 4: 2, 6: 4 };

function isPNG(bytes) {
  if (!bytes || bytes.length < SIGNATURE.length)
    return false;
  for (var i = 0; i < SIGNATURE.length; i++)
    if (bytes[i] !== SIGNATURE[i])
      return false;
  return true;
}

function readUint32(bytes, at) {
  return ((bytes[at] << 24) | (bytes[at + 1] << 16) |
          (bytes[at + 2] << 8) | bytes[at + 3]) >>> 0;
}

/**
 * Undoes the per-scanline filter.
 *
 * Every row is prefixed with a filter byte and encoded against the row above
 * and the pixel to the left, so this has to run in order and cannot be done
 * per row in isolation.
 */
function unfilter(raw, width, height, bpp) {
  var stride = width * bpp;
  var out = new Uint8Array(stride * height);
  var pos = 0;

  for (var y = 0; y < height; y++) {
    var filter = raw[pos++];
    var line = y * stride;
    var prior = line - stride;

    for (var x = 0; x < stride; x++) {
      var value = raw[pos + x];
      var left = x >= bpp ? out[line + x - bpp] : 0;
      var up = y > 0 ? out[prior + x] : 0;
      var upLeft = (y > 0 && x >= bpp) ? out[prior + x - bpp] : 0;

      switch (filter) {
        case 0: break;
        case 1: value += left; break;
        case 2: value += up; break;
        case 3: value += (left + up) >> 1; break;
        case 4:
          // Paeth: pick whichever neighbour the gradient predicts best.
          var p = left + up - upLeft;
          var pa = Math.abs(p - left), pb = Math.abs(p - up), pc = Math.abs(p - upLeft);
          value += (pa <= pb && pa <= pc) ? left : (pb <= pc ? up : upLeft);
          break;
        default:
          throw new Error("PNG filter " + filter);
      }
      out[line + x] = value & 0xFF;
    }
    pos += stride;
  }
  return out;
}

function toRGBA(pixels, width, height, colorType, palette, transparency) {
  var out = new Uint8Array(width * height * 4);
  var bpp = CHANNELS[colorType];

  for (var i = 0, n = width * height; i < n; i++) {
    var src = i * bpp;
    var dst = i * 4;
    var r, g, b, a = 255;

    if (colorType === 0) {              // greyscale
      r = g = b = pixels[src];
    } else if (colorType === 4) {       // greyscale + alpha
      r = g = b = pixels[src];
      a = pixels[src + 1];
    } else if (colorType === 2) {       // truecolour
      r = pixels[src]; g = pixels[src + 1]; b = pixels[src + 2];
    } else if (colorType === 6) {       // truecolour + alpha
      r = pixels[src]; g = pixels[src + 1]; b = pixels[src + 2];
      a = pixels[src + 3];
    } else {                            // indexed
      var index = pixels[src] * 3;
      r = palette[index]; g = palette[index + 1]; b = palette[index + 2];
      if (transparency && pixels[src] < transparency.length)
        a = transparency[pixels[src]];
    }

    out[dst] = r; out[dst + 1] = g; out[dst + 2] = b; out[dst + 3] = a;
  }
  return out;
}

/**
 * @param {Uint8Array} bytes a whole PNG file
 * @returns {{width: number, height: number, data: Uint8Array}} RGBA
 */
function decode(bytes) {
  if (!isPNG(bytes))
    throw new Error("not a PNG");

  var width = 0, height = 0, colorType = 0, palette = null, transparency = null;
  var idat = [];
  var idatLength = 0;
  var at = SIGNATURE.length;

  while (at + 8 <= bytes.length) {
    var length = readUint32(bytes, at);
    var type = String.fromCharCode(bytes[at + 4], bytes[at + 5],
                                   bytes[at + 6], bytes[at + 7]);
    var body = at + 8;

    if (type === "IHDR") {
      width = readUint32(bytes, body);
      height = readUint32(bytes, body + 4);
      var depth = bytes[body + 8];
      colorType = bytes[body + 9];
      if (depth !== 8)
        throw new Error("PNG bit depth " + depth);
      if (bytes[body + 12] !== 0)
        throw new Error("interlaced PNG");
      if (CHANNELS[colorType] === undefined)
        throw new Error("PNG colour type " + colorType);
    } else if (type === "PLTE") {
      palette = bytes.subarray(body, body + length);
    } else if (type === "tRNS") {
      transparency = bytes.subarray(body, body + length);
    } else if (type === "IDAT") {
      // Split across chunks at the encoder's whim; the zlib stream is the
      // concatenation, so it cannot be inflated a chunk at a time.
      idat.push(bytes.subarray(body, body + length));
      idatLength += length;
    } else if (type === "IEND") {
      break;
    }

    at = body + length + 4;  // skip the body and its CRC
  }

  if (!width || !height || !idat.length)
    throw new Error("PNG has no image data");

  var joined = new Uint8Array(idatLength);
  for (var i = 0, offset = 0; i < idat.length; i++) {
    joined.set(idat[i], offset);
    offset += idat[i].length;
  }

  var raw = inflate.inflate(joined);
  var pixels = unfilter(raw, width, height, CHANNELS[colorType]);

  return {
    width: width,
    height: height,
    data: toRGBA(pixels, width, height, colorType, palette, transparency)
  };
}

module.exports = { decode: decode, isPNG: isPNG };
