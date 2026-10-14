// The opt-in mode in which the site's own server provides the game data
// (port/web/site/server-data.js, and xiso-worker.js's download of it): the
// config and manifest checks, the SHA-256, and the download into a stand-in
// for the browser's storage, again only what changed.
// node --test "port/web/tests/*.test.js"

'use strict';

const test = require('node:test');
const assert = require('node:assert');
const crypto = require('node:crypto');
const serverData = require('../site/server-data.js');

const PAGE = 'https://games.example.com/play/index.html';
const MANIFEST = 'https://games.example.com/play/game-data/manifest.json';

const sha = (bytes) => crypto.createHash('sha256').update(bytes).digest('hex');
const entry = (path, bytes) => ({ path, size: bytes.length, sha256: sha(bytes) });

function manifest({ maps, skate } = {}) {
  const value = { format: 'opence-server-data-manifest', version: 1, maps: maps || { base: 'maps/', files: [entry('ui.map', Buffer.from('ui'))] } };
  if (skate) value.skate = skate;
  return value;
}

// ---------- server-data.json

test('a config names its manifest, relative to the page', () => {
  const config = serverData.parseConfig({ format: 'opence-server-data', version: 1, manifest: 'game-data/manifest.json' }, PAGE);
  assert.deepStrictEqual(config, { manifestUrl: MANIFEST });
  assert.strictEqual(serverData.parseConfig({ format: 'opence-server-data', version: 1, manifest: '/data/m.json#x' }, PAGE).manifestUrl,
    'https://games.example.com/data/m.json');
});

test('what is not a config is none (a page served for every address, say)', () => {
  for (const value of [null, 'text', 42, [], {}, { format: 'something else', manifest: 'm.json' }]) {
    assert.strictEqual(serverData.parseConfig(value, PAGE), null);
  }
});

test('a config of another version, or without a manifest, is refused', () => {
  assert.throws(() => serverData.parseConfig({ format: 'opence-server-data', version: 2, manifest: 'm.json' }, PAGE), /version 2/);
  assert.throws(() => serverData.parseConfig({ format: 'opence-server-data', version: 1 }, PAGE), /no address/);
  assert.throws(() => serverData.parseConfig({ format: 'opence-server-data', version: 1, manifest: 7 }, PAGE), /no address/);
});

test('only the page\'s own origin is followed', () => {
  for (const address of ['https://elsewhere.example.org/m.json', '//elsewhere.example.org/m.json', 'http://games.example.com/m.json',
    'https://games.example.com:8443/m.json', 'data:application/json,{}', 'blob:https://games.example.com/x', 'javascript:alert(1)']) {
    assert.throws(() => serverData.parseConfig({ format: 'opence-server-data', version: 1, manifest: address }, PAGE),
      /not this site|is not one/, address);
  }
});

// ---------- the manifest

test('a manifest gives each file its address, under its folder', () => {
  const ui = Buffer.from('ui map');
  const board = Buffer.from('{}');
  const parsed = serverData.parseManifest(manifest({
    maps: { base: 'maps', files: [entry('ui.map', ui), entry('loading.tga', Buffer.from('t'))] },
    skate: { base: '../skate data/', files: [entry('assets/board.json', board), entry('sounds/a b.wav', Buffer.from('w'))] },
  }), MANIFEST);
  assert.strictEqual(parsed.maps.files[0].url, 'https://games.example.com/play/game-data/maps/ui.map');
  assert.strictEqual(parsed.maps.files[0].sha256, sha(ui));
  assert.strictEqual(parsed.maps.bytes, ui.length + 1);
  assert.deepStrictEqual(parsed.skate.files[1].parts, ['sounds', 'a b.wav']);
  assert.strictEqual(parsed.skate.files[1].name, 'a b.wav');
  assert.strictEqual(parsed.skate.files[1].url, 'https://games.example.com/play/skate%20data/sounds/a%20b.wav');
});

test('a manifest without skate data has none; an empty base is the manifest\'s folder', () => {
  const parsed = serverData.parseManifest(manifest({ maps: { base: '', files: [entry('ui.map', Buffer.from('u'))] } }), MANIFEST);
  assert.strictEqual(parsed.skate, null);
  assert.strictEqual(parsed.maps.files[0].url, 'https://games.example.com/play/game-data/ui.map');
});

test('a manifest\'s folders must be on the page\'s origin', () => {
  assert.throws(() => serverData.parseManifest(manifest({ maps: { base: 'https://cdn.example.net/maps/', files: [entry('ui.map', Buffer.from('u'))] } }), MANIFEST),
    /not this site/);
  assert.throws(() => serverData.parseManifest(manifest({ maps: { files: [entry('ui.map', Buffer.from('u'))] } }), MANIFEST), /no address/);
});

test('a manifest\'s paths are checked as the importers check a folder', () => {
  const bad = [
    ['maps', '../ui.map', /plain file name/], ['maps', 'sub/ui.map', /plain file name/], ['maps', '.hidden.map', /plain file name/],
    ['maps', 'readme.txt', /only maps/], ['maps', 'a%2f..map', /plain file name/],
    ['skate', 'board.json', /assets\/ or sounds\//], ['skate', 'other/x.bin', /assets\/ or sounds\//],
    ['skate', 'assets/../x', /plain path/], ['skate', 'assets/.DS_Store', /plain path/], ['skate', 'assets//x', /plain path/],
    ['skate', 'assets/a\\b', /plain path/], ['skate', 'assets/' + 'd/'.repeat(15) + 'x', /assets\/ or sounds\//],
  ];
  for (const [part, path, pattern] of bad) {
    const files = [{ path, size: 1, sha256: sha('x') }];
    const value = part === 'maps' ? manifest({ maps: { base: 'maps/', files } }) : manifest({ skate: { base: 'skate-data/', files } });
    assert.throws(() => serverData.parseManifest(value, MANIFEST), pattern, path);
  }
});

test('a manifest\'s sizes, hashes and lists are checked', () => {
  const one = (file) => manifest({ maps: { base: 'maps/', files: [file] } });
  assert.throws(() => serverData.parseManifest(one({ path: 'ui.map', size: -1, sha256: sha('') }), MANIFEST), /no size/);
  assert.throws(() => serverData.parseManifest(one({ path: 'ui.map', size: 1.5, sha256: sha('') }), MANIFEST), /no size/);
  assert.throws(() => serverData.parseManifest(one({ path: 'ui.map', size: 1, sha256: 'ABC' }), MANIFEST), /no SHA-256/);
  assert.throws(() => serverData.parseManifest(manifest({ maps: { base: 'maps/', files: [] } }), MANIFEST), /no maps/);
  assert.throws(() => serverData.parseManifest(manifest({ maps: { base: 'maps/', files: [entry('ui.map', Buffer.from('a')), entry('ui.map', Buffer.from('b'))] } }), MANIFEST),
    /listed twice/);
  assert.throws(() => serverData.parseManifest({ format: 'opence-server-data-manifest', version: 9, maps: {} }, MANIFEST), /version 9/);
  assert.throws(() => serverData.parseManifest({ maps: {} }, MANIFEST), /format/);
  assert.throws(() => serverData.parseManifest({ format: 'opence-server-data-manifest', version: 1 }, MANIFEST), /no list of files/);
});

// ---------- SHA-256

test('the SHA-256 matches Node\'s, in chunks of any size', () => {
  for (const size of [0, 1, 55, 56, 63, 64, 65, 119, 120, 128, 1000, 70001]) {
    const bytes = crypto.randomBytes(size);
    const hash = new serverData.Sha256();
    for (let at = 0; at < size;) {
      const count = Math.min(size - at, 1 + ((at * 7919) % 131));
      hash.update(bytes.subarray(at, at + count));
      at += count;
    }
    assert.strictEqual(hash.hex(), sha(bytes), `${size} bytes`);
  }
  assert.strictEqual(new serverData.Sha256().update(Buffer.from('abc')).hex(),
    'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad');
});

// ---------- the download (xiso-worker.js), into a stand-in for OPFS

class FakeFile {
  constructor() { this.bytes = new Uint8Array(0); }
  async getFile() {
    const bytes = this.bytes;
    return { size: bytes.length, text: async () => Buffer.from(bytes).toString('utf8') };
  }
  async createSyncAccessHandle() {
    const file = this;
    return {
      truncate(size) { file.bytes = file.bytes.slice(0, size); },
      write(data, { at }) {
        if (at + data.length > file.bytes.length) {
          const grown = new Uint8Array(at + data.length);
          grown.set(file.bytes);
          file.bytes = grown;
        }
        file.bytes.set(data, at);
        return data.length;
      },
      flush() {},
      close() {},
    };
  }
}

class FakeDirectory {
  constructor() { this.kind = 'directory'; this.children = new Map(); }
  async getDirectoryHandle(name, { create } = {}) {
    if (!this.children.has(name)) {
      if (!create) throw new Error('NotFoundError');
      this.children.set(name, new FakeDirectory());
    }
    const child = this.children.get(name);
    if (!(child instanceof FakeDirectory)) throw new Error('TypeMismatchError');
    return child;
  }
  async getFileHandle(name, { create } = {}) {
    if (!this.children.has(name)) {
      if (!create) throw new Error('NotFoundError');
      const file = new FakeFile();
      file.kind = 'file';
      this.children.set(name, file);
    }
    return this.children.get(name);
  }
  async removeEntry(name) {
    if (!this.children.delete(name)) throw new Error('NotFoundError');
  }
  async *entries() { yield* [...this.children.entries()]; }
  read(path) {
    let node = this;
    for (const part of path.split('/')) node = node && node.children.get(part);
    return node ? Buffer.from(node.bytes).toString('utf8') : undefined;
  }
}

function serve(files) {
  const requests = [];
  global.fetch = async (url, options) => {
    requests.push(url);
    assert.strictEqual(options.mode, 'same-origin');
    const path = new URL(url).pathname;
    if (!(path in files)) return new Response('missing', { status: 404 });
    return new Response(files[path]);
  };
  return requests;
}

test('the server\'s files are downloaded into the importers\' places, and later only what changed', async () => {
  const storage = new FakeDirectory();
  Object.defineProperty(globalThis, 'navigator', { value: { storage: { getDirectory: async () => storage } }, configurable: true });
  const messages = [];
  global.postMessage = (message) => messages.push(message);
  const worker = require('../site/xiso-worker.js');

  const files = {
    '/play/game-data/maps/ui.map': 'the ui map',
    '/play/game-data/maps/bloodgulch.map': 'blood gulch',
    '/play/game-data/skate-data/assets/board.json': '{"board":1}',
    '/play/game-data/skate-data/sounds/push.wav': 'RIFF',
  };
  const listing = (prefix) => Object.keys(files).filter((path) => path.startsWith(prefix))
    .map((path) => entry(path.slice(prefix.length), Buffer.from(files[path])));
  const current = () => serverData.parseManifest(manifest({
    maps: { base: 'maps/', files: listing('/play/game-data/maps/') },
    skate: { base: 'skate-data/', files: listing('/play/game-data/skate-data/') },
  }), MANIFEST);

  // the first visit: everything
  let requests = serve(files);
  let result = await worker.syncServer(current(), true);
  assert.strictEqual(requests.length, 4);
  assert.strictEqual(result.maps.downloaded, 2);
  assert.strictEqual(result.skate.downloaded, 2);
  assert.strictEqual(storage.read('maps/ui.map'), 'the ui map');
  assert.strictEqual(storage.read('skate-data/sounds/push.wav'), 'RIFF');
  assert.deepStrictEqual(JSON.parse(storage.read('maps/.complete')),
    { files: ['ui.map', 'bloodgulch.map'], bytes: 21, server: true });
  assert.deepStrictEqual(JSON.parse(storage.read('skate-data/.complete')), { files: 2, bytes: 15, sounds: 1, server: true });
  assert.ok(messages.some((message) => message.type === 'progress' && message.done === message.total));

  // again: nothing
  requests = serve(files);
  result = await worker.syncServer(current(), true);
  assert.deepStrictEqual(requests, []);
  assert.strictEqual(result.bytes, 0);

  // a map changed, one is gone, and the game's copy of the maps goes
  files['/play/game-data/maps/ui.map'] = 'a new ui map';
  delete files['/play/game-data/maps/bloodgulch.map'];
  const drive = await (await storage.getDirectoryHandle('save', { create: true })).getDirectoryHandle('z', { create: true });
  await drive.getFileHandle('cache000.map', { create: true });
  await drive.getFileHandle('profile', { create: true });
  requests = serve(files);
  result = await worker.syncServer(current(), true);
  assert.deepStrictEqual(requests, [MANIFEST.replace('manifest.json', 'maps/ui.map')]);
  assert.strictEqual(result.maps.removed, 1);
  assert.strictEqual(storage.read('maps/ui.map'), 'a new ui map');
  assert.strictEqual(storage.read('maps/bloodgulch.map'), undefined);
  assert.deepStrictEqual([...drive.children.keys()], ['profile']);

  // a file the player's storage lost is downloaded again
  (await storage.getDirectoryHandle('skate-data')).children.get('assets').children.delete('board.json');
  requests = serve(files);
  await worker.syncServer(current(), true);
  assert.deepStrictEqual(requests, [MANIFEST.replace('manifest.json', 'skate-data/assets/board.json')]);

  // a file that does not match its hash fails, and the folder is not ready
  files['/play/game-data/maps/ui.map'] = 'tampered!!!!';
  const stale = current();
  stale.maps.files[0].sha256 = sha('something else');
  serve(files);
  await assert.rejects(worker.syncServer(stale, true), /does not match its manifest's SHA-256/);
  assert.strictEqual(storage.read('maps/.complete'), undefined);

  // the server not giving a file fails
  serve({});
  await assert.rejects(worker.syncServer(current(), true), /HTTP 404/);

  // without Skate 3 mode in the build, the skate data stays where it is
  serve(files);
  result = await worker.syncServer(current(), false);
  assert.strictEqual(result.skate, null);
});
