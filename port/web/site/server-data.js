/*
SERVER-DATA.JS

The opt-in mode in which the site's own server provides the game data, for
a private server of the owner's own copy (port/web/README.md, "Serving the
game data privately"): no player then imports anything.

At start the page (app.js) asks for server-data.json beside index.html. The
public site and the release zips have none (tools/web_build.py never copies
one), so there it is a 404 and nothing changes. A server that has one names a
manifest of the files (tools/web_server_data_manifest.py writes both):

  server-data.json  {"format": "opence-server-data", "version": 1,
                     "manifest": "game-data/manifest.json"}
  the manifest      {"format": "opence-server-data-manifest", "version": 1,
                     "maps": {"base": "maps/", "files": [{"path": "ui.map", "size": 1234,
                                                          "sha256": "<64 hex digits>"}, ...]},
                     "skate": {"base": "skate-data/", "files": [{"path": "assets/board.json", ...}]}}

The manifest's address is relative to the page, each base to the manifest
and each path to its base. The importer (xiso-worker.js) then downloads the
files into the browser's storage where its own imports put them (maps/, and
skate-data/assets and skate-data/sounds), checks each one's size and
SHA-256, and on a later visit downloads only those whose hash changed.

Only the page's own origin is followed: an address of another (or a
redirect to one) is refused, so a config cannot pull data from elsewhere.
What a manifest names is checked as the importers check a player's folder:
plain names, the maps folder's .map files and loading.tga, skate data under
assets/ and sounds/.

Exports, for app.js and xiso-worker.js (self.HaloServerData) and the tests
(port/web/tests/server-data.test.js): parseConfig, parseManifest,
sameOriginUrl, Sha256, and the names and formats.
*/

'use strict';

(function (exports) {
  const CONFIG_NAME = 'server-data.json';
  const CONFIG_FORMAT = 'opence-server-data';
  const MANIFEST_FORMAT = 'opence-server-data-manifest';
  const FORMAT_VERSION = 1;
  // as xiso-worker.js: a maps folder's names, skate data's names and depth
  const MAXIMUM_MAP_FILES = 256;
  const MAXIMUM_SKATE_FILES = 20000;
  const MAXIMUM_SKATE_DEPTH = 16;
  const SKATE_PARTS = ['assets', 'sounds'];

  function mapName(name) {
    return name.length > 0 && name.length <= 64 && /^[A-Za-z0-9_.\- ]+$/.test(name) && !name.startsWith('.');
  }

  function mapWanted(name) {
    const lower = name.toLowerCase();
    return mapName(name) && (lower.endsWith('.map') || lower === 'loading.tga');
  }

  function skateName(name) {
    return name.length > 0 && name.length <= 255 && !name.startsWith('.') && !/[\\/\0-\x1f]/.test(name);
  }

  // an address, relative to base, that must be of base's origin
  function sameOriginUrl(text, base, what) {
    if (typeof text !== 'string' || text === '') throw new Error(`${what} has no address`);
    let url;
    try {
      url = new URL(text, base);
    } catch {
      throw new Error(`${what}'s address, ${JSON.stringify(text)}, is not one`);
    }
    const origin = new URL(base).origin;
    // (a blob: address has its creator's origin, but is not the server's)
    if (url.origin !== origin || origin === 'null' || !/^https?:$/.test(url.protocol)) {
      throw new Error(`${what} is at ${url.origin}, not this site (${origin}): only the site's own addresses are followed`);
    }
    url.hash = '';
    return url;
  }

  // server-data.json's contents: { manifestUrl }, or null when it is not one
  // (a server that answers every address with its page, say); an error when
  // it is one this build cannot follow
  function parseConfig(value, pageUrl) {
    if (!value || typeof value !== 'object' || value.format !== CONFIG_FORMAT) return null;
    if (value.version !== FORMAT_VERSION) {
      throw new Error(`${CONFIG_NAME} is version ${JSON.stringify(value.version)}, and this build reads version ${FORMAT_VERSION}`);
    }
    return { manifestUrl: sameOriginUrl(value.manifest, pageUrl, 'The manifest').href };
  }

  function checkedPart(part, name, manifestUrl, maximum, checkPath) {
    if (!part || typeof part !== 'object' || !Array.isArray(part.files)) {
      throw new Error(`The manifest's ${name} has no list of files`);
    }
    if (part.files.length > maximum) throw new Error(`The manifest lists ${part.files.length} files of ${name}, more than ${maximum}`);
    // (a folder: its address ends in /; an empty one is the manifest's own)
    const folder = part.base === '' ? './' : typeof part.base === 'string' ? part.base.replace(/\/*$/, '/') : part.base;
    const base = sameOriginUrl(folder, manifestUrl, `The manifest's ${name}`);
    base.search = '';
    const seen = new Set();
    const files = part.files.map((file) => {
      if (!file || typeof file !== 'object' || typeof file.path !== 'string') throw new Error(`A file of ${name} has no path`);
      const parts = file.path.split('/');
      const problem = checkPath(parts);
      if (problem) throw new Error(`${JSON.stringify(file.path)} in ${name}: ${problem}`);
      if (seen.has(file.path)) throw new Error(`${JSON.stringify(file.path)} is listed twice in ${name}`);
      seen.add(file.path);
      if (!Number.isSafeInteger(file.size) || file.size < 0) throw new Error(`${file.path} has no size`);
      if (typeof file.sha256 !== 'string' || !/^[0-9a-f]{64}$/.test(file.sha256)) throw new Error(`${file.path} has no SHA-256`);
      const url = sameOriginUrl(parts.map(encodeURIComponent).join('/'), base.href, file.path);
      if (!url.pathname.startsWith(base.pathname)) throw new Error(`${file.path} is not in its folder`);
      return { path: file.path, parts, name: parts[parts.length - 1], size: file.size, sha256: file.sha256, url: url.href };
    });
    return { files, bytes: files.reduce((sum, file) => sum + file.size, 0) };
  }

  function mapPath(parts) {
    if (parts.length !== 1 || !mapName(parts[0])) return 'not a plain file name';
    if (!mapWanted(parts[0])) return 'only maps and loading.tga go in the maps folder';
    return null;
  }

  function skatePath(parts) {
    if (parts.length < 2 || parts.length > MAXIMUM_SKATE_DEPTH || !SKATE_PARTS.includes(parts[0])) {
      return 'not in assets/ or sounds/';
    }
    if (!parts.every(skateName)) return 'not a plain path';
    return null;
  }

  // the manifest's contents, checked: { maps, skate } (skate null when it
  // has none), each { files: [{ path, parts, name, size, sha256, url }], bytes }
  function parseManifest(value, manifestUrl) {
    if (!value || typeof value !== 'object' || value.format !== MANIFEST_FORMAT) {
      throw new Error('The manifest is not one (its format is not ' + MANIFEST_FORMAT + ')');
    }
    if (value.version !== FORMAT_VERSION) {
      throw new Error(`The manifest is version ${JSON.stringify(value.version)}, and this build reads version ${FORMAT_VERSION}`);
    }
    const maps = checkedPart(value.maps, 'maps', manifestUrl, MAXIMUM_MAP_FILES, mapPath);
    if (!maps.files.length) throw new Error('The manifest lists no maps');
    const skate = value.skate == null ? null : checkedPart(value.skate, 'skate', manifestUrl, MAXIMUM_SKATE_FILES, skatePath);
    return { maps, skate };
  }

  // ---------- SHA-256 (FIPS 180-4), a chunk at a time: SubtleCrypto hashes
  // only a whole buffer, and a map is hundreds of MB

  const K = new Uint32Array([
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
  ]);

  class Sha256 {
    constructor() {
      this.state = new Uint32Array([0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19]);
      this.block = new Uint8Array(64);
      this.used = 0;
      this.length = 0;
      this.words = new Uint32Array(64);
    }

    update(bytes) {
      const count = bytes.length;
      let at = 0;
      this.length += count;
      if (this.used) {
        at = Math.min(64 - this.used, count);
        this.block.set(bytes.subarray(0, at), this.used);
        this.used += at;
        if (this.used < 64) return this;
        this.compress(this.block, 0);
        this.used = 0;
      }
      for (; at + 64 <= count; at += 64) this.compress(bytes, at);
      if (at < count) {
        this.block.set(bytes.subarray(at), 0);
        this.used = count - at;
      }
      return this;
    }

    compress(bytes, offset) {
      const w = this.words;
      const h = this.state;
      for (let t = 0; t < 16; t++, offset += 4) {
        w[t] = bytes[offset] << 24 | bytes[offset + 1] << 16 | bytes[offset + 2] << 8 | bytes[offset + 3];
      }
      for (let t = 16; t < 64; t++) {
        const x = w[t - 15];
        const y = w[t - 2];
        const s0 = (x >>> 7 | x << 25) ^ (x >>> 18 | x << 14) ^ (x >>> 3);
        const s1 = (y >>> 17 | y << 15) ^ (y >>> 19 | y << 13) ^ (y >>> 10);
        w[t] = w[t - 16] + s0 + w[t - 7] + s1;
      }
      let a = h[0]; let b = h[1]; let c = h[2]; let d = h[3];
      let e = h[4]; let f = h[5]; let g = h[6]; let k = h[7];
      for (let t = 0; t < 64; t++) {
        const s1 = (e >>> 6 | e << 26) ^ (e >>> 11 | e << 21) ^ (e >>> 25 | e << 7);
        const t1 = (k + s1 + ((e & f) ^ (~e & g)) + K[t] + w[t]) | 0;
        const s0 = (a >>> 2 | a << 30) ^ (a >>> 13 | a << 19) ^ (a >>> 22 | a << 10);
        const t2 = (s0 + ((a & b) ^ (a & c) ^ (b & c))) | 0;
        k = g; g = f; f = e; e = (d + t1) | 0;
        d = c; c = b; b = a; a = (t1 + t2) | 0;
      }
      h[0] += a; h[1] += b; h[2] += c; h[3] += d;
      h[4] += e; h[5] += f; h[6] += g; h[7] += k;
    }

    // the digest as 64 hex digits (the hash is finished: update no more)
    hex() {
      const length = this.length;
      const tail = new Uint8Array(this.used < 56 ? 64 - this.used : 128 - this.used);
      tail[0] = 0x80;
      const view = new DataView(tail.buffer);
      view.setUint32(tail.length - 8, Math.floor(length / 0x20000000));
      view.setUint32(tail.length - 4, (length % 0x20000000) * 8);
      this.update(tail);
      return [...this.state].map((word) => word.toString(16).padStart(8, '0')).join('');
    }
  }

  Object.assign(exports, {
    CONFIG_NAME, CONFIG_FORMAT, MANIFEST_FORMAT, FORMAT_VERSION, parseConfig, parseManifest, sameOriginUrl, Sha256,
  });
})(typeof module !== 'undefined' && module.exports ? module.exports : (self.HaloServerData = {}));
