// The opt-in mode in which the site's own server provides the game data
// (port/web/site/server-data.js; port/web/README.md, "Serving the game data
// privately"), in a headless Chromium:
//
//     node port/web/tests/server-data.mjs dist/halo-web-release
//     node port/web/tests/server-data.mjs --url http://localhost:8080/ [--shot menu.png]
//
// (with Playwright where Node finds it, and python3.) The first form makes a
// small stand-in data set (placeholder files: no game data is needed), its
// manifest and server-data.json with tools/web_server_data_manifest.py, and
// serves them beside the site with tools/web_serve.py. In one browser
// profile it checks that a server-data.json naming another origin is
// refused (the imports stay); that with a good one the page downloads the
// files on its own, without its import buttons, into where the importers
// put them (each file read back and compared); that a second visit
// downloads nothing and a changed file alone is downloaded again; that the
// game starts with them (as the smoke test, it looks for the menus' map,
// which the stand-in set has none of); and that without server-data.json
// the import buttons are back. About a minute.
//
// The second form tests a server that is already up with the real data
// (the owner's own, on a private network): the page imports it on its own,
// the game reaches its main menu, then hosts a game alone on Blood Gulch,
// and in a build with Skate 3 mode J must put the player on the board
// (skate: on). --shot saves the page at the menu and on the board. A few
// minutes, most of it the download of the maps (2 GB).

import { spawn, spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { mkdirSync, mkdtempSync, readdirSync, rmSync, symlinkSync, writeFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { setTimeout as sleep } from 'node:timers/promises';

const require = createRequire(import.meta.url);
const { chromium } = require('playwright');

const here = path.dirname(new URL(import.meta.url).pathname);
const root = path.resolve(here, '../../..');
const args = process.argv.slice(2);
const option = (name) => {
  const index = args.indexOf(name);
  return index >= 0 ? args.splice(index, 2)[1] : null;
};
const serverUrl = option('--url');
const shot = option('--shot');
const work = mkdtempSync(path.join(tmpdir(), 'opence-server-data-'));
const children = [];
const lines = [];

function fail(message) {
  console.error('FAILED: ' + message);
  process.exitCode = 1;
}

async function until(test, seconds) {
  for (let waited = 0; waited < seconds * 4 && !(await test()); waited++) await sleep(250);
  return test();
}

const sha = (data) => createHash('sha256').update(data).digest('hex');

// the page's files in the browser's storage, each path to its SHA-256
async function storedFiles(page) {
  const files = await page.evaluate(async () => {
    const found = {};
    const walk = async (directory, prefix) => {
      for await (const [name, handle] of directory.entries()) {
        if (handle.kind === 'directory') await walk(handle, prefix + name + '/');
        else found[prefix + name] = Array.from(new Uint8Array(await (await handle.getFile()).arrayBuffer()));
      }
    };
    const storage = await navigator.storage.getDirectory();
    for (const folder of ['maps', 'skate-data']) {
      try {
        await walk(await storage.getDirectoryHandle(folder), folder + '/');
      } catch {
        // none
      }
    }
    return found;
  });
  return Object.fromEntries(Object.entries(files).map(([name, bytes]) => [name, sha(Buffer.from(bytes))]));
}

function launch(profile) {
  return chromium.launchPersistentContext(profile, {
    headless: true, viewport: { width: 960, height: 720 },
    args: ['--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist'],
  });
}

function listen(page) {
  page.on('console', (message) => {
    for (const line of message.text().split('\n')) if (line) lines.push(line);
  });
  page.on('pageerror', (error) => lines.push('page error: ' + error));
}

const has = (pattern) => lines.some((line) => pattern.test(line));

async function waitForLauncher(page) {
  await page.waitForFunction(() => document.querySelectorAll('#checks li').length >= 5, null, { timeout: 60000 });
}

async function waitForData(page, seconds) {
  await page.waitForFunction(() => /ready|cannot|could not/.test(document.querySelector('#data-status').textContent) &&
    !/Checking|Downloading/.test(document.querySelector('#import-text').textContent), null, { timeout: seconds * 1000 });
  return { status: await page.textContent('#data-status'), text: await page.textContent('#import-text') };
}

// ---------- a stand-in data set, served beside the site

async function standIn(site) {
  const served = path.join(work, 'site');
  mkdirSync(served);
  for (const name of readdirSync(site)) symlinkSync(path.join(site, name), path.join(served, name));
  const data = path.join(served, 'game-data');
  const files = {
    'maps/placeholder.map': 'not a real map',
    'maps/loading.tga': 'not a real image',
    'skate-data/assets/board.json': '{"placeholder": true}',
    'skate-data/assets/meshes/deck.bin': 'deck',
    'skate-data/sounds/push.wav': 'not a real sound',
    // (beside the skate data, not under assets/ or sounds/: not listed)
    'skate-data/conversion.log': 'log',
  };
  const write = (name, text) => {
    mkdirSync(path.dirname(path.join(data, name)), { recursive: true });
    writeFileSync(path.join(data, name), text);
  };
  for (const [name, text] of Object.entries(files)) write(name, text);
  const manifest = () => {
    const result = spawnSync('python3', [path.join(root, 'tools/web_server_data_manifest.py'), '--maps', path.join(data, 'maps'),
      '--skate-data', path.join(data, 'skate-data'), '--output', path.join(data, 'manifest.json'),
      '--config', path.join(served, 'server-data.json'), '--manifest-url', 'game-data/manifest.json'], { encoding: 'utf8' });
    if (result.status !== 0) throw new Error('web_server_data_manifest.py: ' + result.stderr);
  };
  manifest();
  const port = 8000 + Math.floor(Math.random() * 1000);
  const server = spawn('python3', [path.join(root, 'tools/web_serve.py'), served, '--port', String(port)],
    { stdio: ['ignore', 'ignore', 'pipe'] });
  children.push(server);
  // (what the browser asked of the game data, from the server's log)
  const requests = [];
  server.stderr.on('data', (chunk) => {
    for (const match of String(chunk).matchAll(/"GET \/game-data\/([^ ?]+)[^"]*" (\d+)/g)) requests.push(`${match[1]} ${match[2]}`);
  });
  await sleep(1500);
  return { served, data, files, write, manifest, requests, port, url: `http://localhost:${port}/` };
}

async function testStandIn(site) {
  const set = await standIn(site);
  const expected = () => Object.fromEntries(Object.entries(set.files).filter(([name]) => !name.endsWith('conversion.log'))
    .map(([name, text]) => [name, sha(text)]));
  const context = await launch(path.join(work, 'profile'));
  try {
    const page = context.pages()[0] || await context.newPage();
    listen(page);

    // a config that names another origin (127.0.0.1 is not localhost's): refused
    writeFileSync(path.join(set.served, 'server-data.json'), JSON.stringify({ format: 'opence-server-data', version: 1,
      manifest: `http://127.0.0.1:${set.port}/game-data/manifest.json` }));
    await page.goto(set.url);
    await waitForLauncher(page);
    await page.waitForFunction(() => /cannot be used/.test(document.querySelector('#import-text').textContent), null, { timeout: 30000 });
    console.log('another origin: ' + await page.textContent('#import-text'));
    if (!/not this site/.test(await page.textContent('#import-text'))) fail('a config naming another origin was not refused as such');
    if (await page.isHidden('#image-label')) fail('the imports were hidden for a config that was refused');
    if (set.requests.length) fail('the page asked for game data it should not have: ' + set.requests.join(', '));

    // the good one: downloaded without the player
    set.manifest();
    let started = Date.now();
    await page.goto(set.url);
    await waitForLauncher(page);
    let data = await waitForData(page, 60);
    console.log(`first visit: ${data.status} ${data.text} (${((Date.now() - started) / 1000).toFixed(1)} s)`);
    if (!/ready/.test(data.status)) fail('the game data was not downloaded: ' + data.text);
    for (const id of ['#image-label', '#folder-label', '#skate-label']) {
      if (!(await page.isHidden(id))) fail(`${id} is shown although the server provides the data`);
    }
    const skate = await page.textContent('#skate-status');
    console.log('skate: ' + skate);
    if (!/3 files of skate data .*with 1 sound files, ready/.test(skate)) fail('the skate data was not downloaded');
    const stored = await storedFiles(page);
    for (const [name, digest] of Object.entries(expected())) {
      if (stored[name] !== digest) fail(`${name} in the browser's storage is not the server's`);
    }
    if (Object.keys(stored).some((name) => /conversion\.log/.test(name))) fail('a file the manifest does not list was copied');
    const downloads = () => set.requests.filter((request) => !request.startsWith('manifest.json'));
    console.log('downloaded: ' + downloads().join(', '));
    if (downloads().length !== 5) fail(`the first visit downloaded ${downloads().length} files, not 5`);

    // again: nothing but the manifest
    set.requests.length = 0;
    await page.reload();
    await waitForLauncher(page);
    data = await waitForData(page, 60);
    console.log(`second visit: ${data.text}`);
    if (downloads().length) fail('the second visit downloaded ' + downloads().join(', '));
    if (!/Up to date/.test(data.text)) fail('the second visit did not say the data is up to date');

    // a changed file alone
    set.files['skate-data/sounds/push.wav'] = 'a different placeholder sound';
    set.write('skate-data/sounds/push.wav', set.files['skate-data/sounds/push.wav']);
    set.manifest();
    set.requests.length = 0;
    await page.reload();
    await waitForLauncher(page);
    data = await waitForData(page, 60);
    console.log(`after a change: ${data.text}; downloaded ${downloads().join(', ')}`);
    if (downloads().join() !== 'skate-data/sounds/push.wav 200') fail('the changed file alone was not downloaded again');
    if ((await storedFiles(page))['skate-data/sounds/push.wav'] !== sha(set.files['skate-data/sounds/push.wav'])) {
      fail('the changed file in the browser\'s storage is not the new one');
    }

    // the game starts with it (and looks for the menus' map, which is not
    // among the placeholders)
    await page.waitForFunction(() => !document.querySelector('#play').disabled, null, { timeout: 30000 });
    await page.click('#play');
    started = Date.now();
    await until(() => has(/couldn't find map 'ui'/), 120);
    if (!has(/data root: \/data/)) fail('the game did not mount the storage at /data');
    if (!has(/couldn't find map 'ui'/)) fail('the game did not get as far as the maps');
    if (!(await page.isHidden('#fatal'))) fail('the game stopped: ' + (await page.textContent('#fatal-text')));
    console.log(`the game started with the server's data (${((Date.now() - started) / 1000).toFixed(1)} s)`);

    // without server-data.json, as the public site: the imports are back
    rmSync(path.join(set.served, 'server-data.json'));
    await page.goto(set.url);
    await waitForLauncher(page);
    await page.waitForFunction(() => !/Looking/.test(document.querySelector('#data-status').textContent));
    if (await page.isHidden('#image-label') || await page.isHidden('#folder-label')) fail('without server-data.json the imports are hidden');
    if (!(await page.isHidden('#import-progress'))) fail('without server-data.json the page still said something of it');
    console.log('without server-data.json: the imports are back');
  } finally {
    await context.close().catch(() => {});
  }
  if (!process.exitCode) console.log('server data in a browser: ok');
}

// ---------- a server already up, with the real data

async function testServer(url) {
  const context = await launch(path.join(work, 'profile'));
  try {
    const page = context.pages()[0] || await context.newPage();
    listen(page);
    let started = Date.now();
    await page.goto(url);
    await waitForLauncher(page);
    const checks = await page.$$eval('#checks li', (items) => items.map((item) => `${item.className}: ${item.textContent}`));
    console.log(checks.join('\n'));
    const data = await waitForData(page, 1200);
    console.log(`data: ${data.status} ${data.text} (${((Date.now() - started) / 1000).toFixed(1)} s)`);
    console.log('skate: ' + await page.textContent('#skate-status'));
    if (!/ready/.test(data.status)) throw new Error('the game data was not downloaded: ' + data.text);
    if (!(await page.isHidden('#image-label'))) fail('the imports are shown although the server provides the data');

    // the main menu: the game runs on with ui.map, nothing stops it
    await page.click('#play');
    started = Date.now();
    const frames = () => page.evaluate(() => {
      /* global game */
      return typeof game !== 'undefined' && game && game._web_state ? game.HEAPU32[game._web_state() >> 2] : 0;
    });
    if (!await until(async () => (await frames()) > 300, 300)) fail('the game did not get going');
    if (has(/couldn't find map/)) fail('the game did not find a map');
    if (!(await page.isHidden('#fatal'))) throw new Error('the game stopped: ' + (await page.textContent('#fatal-text')));
    console.log(`the main menu: ${await frames()} frames shown (${((Date.now() - started) / 1000).toFixed(1)} s)`);
    if (shot) await page.screenshot({ path: shot.replace(/(\.png)?$/, '-menu.png') });

    // alone on Blood Gulch, then J
    lines.length = 0;
    await page.goto(`${url}?set=HALO_NETWORK_TEST=host:bloodgulch&set=HALO_SKATE_STATUS=1&set=HALO_NET_ONLINE=false`);
    await waitForLauncher(page);
    await waitForData(page, 300);
    await page.waitForFunction(() => !document.querySelector('#play').disabled, null, { timeout: 60000 });
    await page.click('#play');
    const ticks = () => lines.filter((line) => /network test: tick \d+ .*player 0: \(/.test(line)).length;
    if (!await until(() => ticks() >= 3, 300)) throw new Error('the game did not start on Blood Gulch');
    console.log('on Blood Gulch');
    await page.click('#canvas');
    await page.keyboard.down('KeyJ');
    await sleep(150);
    await page.keyboard.up('KeyJ');
    if (!await until(() => has(/skate: (on|off)/), 240)) fail('J did nothing');
    for (const line of lines.filter((text) => /^skate: |halo-skate: preloading/.test(text)).slice(0, 4)) console.log('log: ' + line);
    if (!has(/skate: on/)) fail('J did not put the player on the board');
    await sleep(3000);
    if (shot) await page.screenshot({ path: shot.replace(/(\.png)?$/, '-skate.png') });
  } finally {
    await context.close().catch(() => {});
  }
  if (!process.exitCode) console.log('a server\'s own data in a browser: ok');
}

try {
  if (serverUrl) await testServer(serverUrl);
  else await testStandIn(path.resolve(args[0] || 'build/web/site'));
} catch (error) {
  fail(String(error && error.stack ? error.stack : error));
} finally {
  if (process.exitCode) console.error('the page logged:\n' + lines.slice(-40).join('\n'));
  for (const child of children) child.kill('SIGKILL');
  rmSync(work, { recursive: true, force: true });
}
