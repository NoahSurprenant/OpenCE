// Skate 3 mode in a browser (port/skate/README.md, "In a browser"):
//
//     node port/web/tests/skate.mjs <site> <maps folder> <skate-data folder> [<screenshot.png>]
//     node port/web/tests/skate.mjs <site> <maps folder> none
//
// (with Playwright where Node finds it; the site built with --skate; the maps
// folder holds ui.map and bloodgulch.map, the skate-data folder the
// converted Skate 3 data, assets/ and sounds/: the player's own, which only
// this browser's throwaway profile imports). The page imports both, through
// its two importers, then hosts a system link game on Blood Gulch alone
// (debug.network_test host, which logs the player each second); once the
// player is in the game the test presses J on the keyboard, as a player
// would, and the log must show the skater preloaded and the level sent to
// the engine and its rails found, the player on the board, the pose finite
// and the board drawn (debug.skate_status, each second), and the sounds
// loaded from the imported folder. It rolls, carves and steps off, then
// both sticks clicked in on a controller (a stand-in through the Gamepad
// API) get off. With a screenshot path it saves the page on the board,
// rolling, carving and stepped off. With none for the skate data it imports
// none, and J must say there is none and the game play on. It reports the
// frame rate (frames shown a second, read from the page) and the browser's
// memory, before and after J. About two minutes, most of it the imports and
// the skater's preload (in a software renderer, as headless Chromium has).
//
// HALO_SKATE_TEST_CHROMIUM names a Chromium to run instead of Playwright's;
// HALO_SKATE_TEST_LOG a file for the game's whole log; HALO_SKATE_TEST_SET
// more settings for the game (NAME=value,...).

import { spawn } from 'node:child_process';
import { copyFileSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, rmSync, writeFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { setTimeout as sleep } from 'node:timers/promises';

const require = createRequire(import.meta.url);
const { chromium } = require('playwright');

const here = path.dirname(new URL(import.meta.url).pathname);
const root = path.resolve(here, '../../..');
const site = path.resolve(process.argv[2] || 'build/web/site');
const maps = path.resolve(process.argv[3] || 'assets/maps');
const withoutData = process.argv[4] === 'none';
const skateData = path.resolve(process.argv[4] || 'skate-data');
const screenshot = process.argv[5] ? path.resolve(process.argv[5]) : '';
const sitePort = 8000 + Math.floor(Math.random() * 1000);
const work = mkdtempSync(path.join(tmpdir(), 'opence-skate-'));
const children = [];

function fail(message) {
  console.error('FAILED: ' + message);
  process.exitCode = 1;
}

async function until(test, seconds) {
  for (let waited = 0; waited < seconds && !test(); waited++) await sleep(1000);
  return test();
}

// the browser's memory: the proportional set size of its processes (the one
// started with this profile and all under it), in MB (Linux)
function browserMemory() {
  const parents = new Map();
  const ours = new Set();
  for (const pid of readdirSync('/proc').filter((name) => /^\d+$/.test(name))) {
    try {
      // (the parent is the second field after the command's parenthesis)
      const stat = readFileSync(`/proc/${pid}/stat`, 'latin1');
      parents.set(pid, stat.slice(stat.lastIndexOf(')') + 2).split(' ')[1]);
      if (readFileSync(`/proc/${pid}/cmdline`, 'latin1').includes(work)) ours.add(pid);
    } catch {
      // (gone)
    }
  }
  let total = 0;
  for (const pid of parents.keys()) {
    let up = pid;
    for (let depth = 0; up && up !== '0' && !ours.has(up) && depth < 64; depth++) up = parents.get(up);
    if (!up || !ours.has(up)) continue;
    try {
      const match = /^Pss:\s+(\d+) kB/m.exec(readFileSync(`/proc/${pid}/smaps_rollup`, 'latin1'));
      if (match) total += Number(match[1]);
    } catch {
      // (gone, or not ours to read)
    }
  }
  return total / 1024;
}
let context = null;
const lines = [];
try {
  children.push(spawn('python3', [path.join(root, 'tools/web_serve.py'), site, '--port', String(sitePort)],
    { stdio: 'ignore', env: { ...process.env, WEB_SERVE_QUIET: '1' } }));
  await sleep(1500);
  const pageUrl = `http://localhost:${sitePort}/`;
  const imported = path.join(work, 'maps');
  mkdirSync(imported);
  for (const map of ['ui.map', 'bloodgulch.map']) copyFileSync(path.join(maps, map), path.join(imported, map));
  context = await chromium.launchPersistentContext(path.join(work, 'profile'), {
    headless: true, viewport: { width: 960, height: 720 },
    ...(process.env.HALO_SKATE_TEST_CHROMIUM ? { executablePath: process.env.HALO_SKATE_TEST_CHROMIUM } : {}),
    args: ['--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist'],
  });
  // a controller the page sees through the Gamepad API (navigator.getGamepads
  // and gamepadconnected), with the standard mapping, for the stick clicks:
  // SDL's Emscripten joystick makes buttons 10 and 11 the sticks
  await context.addInitScript(() => {
    const button = (down) => ({ pressed: down, touched: down, value: down ? 1 : 0 });
    const pad = { id: 'OpenCE test pad (STANDARD GAMEPAD)', index: 0, connected: true, mapping: 'standard',
      timestamp: 1, axes: [0, 0, 0, 0], buttons: Array.from({ length: 17 }, () => button(false)) };
    let connected = false;
    navigator.getGamepads = () => (connected ? [pad, null, null, null] : [null, null, null, null]);
    window.testPad = {
      connect() {
        connected = true;
        const event = new Event('gamepadconnected');
        event.gamepad = pad;
        window.dispatchEvent(event);
      },
      sticks(down) {
        pad.buttons[10] = button(down);
        pad.buttons[11] = button(down);
        pad.timestamp = performance.now();
      },
    };
  });
  const page = context.pages()[0] || await context.newPage();
  await page.goto(pageUrl);
  await page.waitForFunction(() => document.querySelectorAll('#checks li').length >= 5, null, { timeout: 60000 });
  await page.waitForSelector('#skate-panel', { state: 'visible', timeout: 30000 }).catch(() => {
    throw new Error('the page offers no skate data import: is the site built with --skate?');
  });

  // the imports: the maps, and the skate data unless none
  let started = Date.now();
  await page.setInputFiles('#folder-input', imported);
  await page.waitForFunction(() => /Copied|cannot|failed/.test(document.querySelector('#import-text').textContent), null,
    { timeout: 600000 });
  console.log(`maps: ${await page.textContent('#data-status')} (${((Date.now() - started) / 1000).toFixed(1)} s)`);
  if (withoutData) {
    console.log(`skate data: ${await page.textContent('#skate-status')}`);
  } else {
    started = Date.now();
    await page.setInputFiles('#skate-input', skateData);
    await page.waitForFunction(() => /Copied|no assets|failed|keep/.test(document.querySelector('#skate-text').textContent),
      null, { timeout: 600000 });
    // (the panel reads the copy back after it says so)
    await page.waitForFunction(() => /ready/.test(document.querySelector('#skate-status').textContent), null,
      { timeout: 15000 }).catch(() => {});
    const skateStatus = await page.textContent('#skate-status');
    console.log(`skate data: ${skateStatus} (${((Date.now() - started) / 1000).toFixed(1)} s)`);
    if (!/ready/.test(skateStatus)) throw new Error('the skate data was not imported: ' + await page.textContent('#skate-text'));
  }

  // a game alone on Blood Gulch, the skater's state each second (and
  // HALO_SKATE_TEST_SET's settings, NAME=value,...: HALO_GL_DEBUG=1, say)
  page.on('console', (message) => {
    for (const line of message.text().split('\n')) {
      if (!line) continue;
      lines.push(line);
      if (/halo-skate: (?!sound: \w+ from)|skate: |network test: hosting/.test(line)) console.log(`  | ${line.slice(0, 200)}`);
    }
  });
  page.on('pageerror', (error) => console.log(`  page error: ${error}`));
  const extra = (process.env.HALO_SKATE_TEST_SET || '').split(',').filter(Boolean)
    .map((setting) => `&set=${encodeURIComponent(setting)}`).join('');
  await page.goto(`${pageUrl}?set=HALO_NETWORK_TEST=host:bloodgulch&set=HALO_SKATE_STATUS=1&set=HALO_NET_ONLINE=false${extra}`);
  await page.waitForFunction(() => !document.querySelector('#play').disabled, null, { timeout: 60000 });
  await page.click('#play');
  const has = (pattern) => lines.some((line) => pattern.test(line));
  const ticks = () => lines.filter((line) => /network test: tick \d+ .*player 0: \(/.test(line)).length;
  if (!await until(() => ticks() >= 3, 300)) throw new Error('the game did not start on Blood Gulch');
  await page.evaluate(() => window.testPad.connect());
  if (!withoutData && !await until(() => has(/halo-skate: map \d+ sent, \d+ triangles/) && has(/halo-skate: map ready/), 240)) {
    fail('the level was not sent to the skate engine');
  }

  const frameRate = async (seconds) => {
    const read = () => page.evaluate(() => {
      // (app.js's game, the module: web_state's first word, the frames shown)
      /* global game */
      return typeof game !== 'undefined' && game && game._web_state ? game.HEAPU32[game._web_state() >> 2] : -1;
    });
    const first = await read();
    await sleep(seconds * 1000);
    const last = await read();
    return first < 0 ? null : (last - first) / seconds;
  };
  const measure = async () => ({ fps: await frameRate(5), memory: browserMemory() });
  const before = await measure();
  const press = async (key) => {
    await page.keyboard.down(key);
    await sleep(150);
    await page.keyboard.up(key);
  };
  const shoot = async (name) => {
    if (!screenshot) return;
    const file = name ? screenshot.replace(/(\.png)?$/, `-${name}.png`) : screenshot;
    await page.screenshot({ path: file });
    console.log(`screenshot: ${file}`);
  };

  // J, as a player presses it
  await page.click('#canvas');
  await press('KeyJ');
  let during = before;
  if (withoutData) {
    // (the game says so, as on the desktop, and plays on)
    if (!await until(() => has(/skate: off, the skate engine failed: .*(no Skate 3 data|assets)/i), 30)) {
      fail('J without skate data did not say there is none');
    }
    if (has(/skate: on/)) fail('J without skate data put the player on a board');
    if (!await until(() => ticks() >= 6, 30)) fail('the game did not go on after J');
    during = await measure();
  } else {
    const statuses = () => lines.filter((line) =>
      /skate status: .* on the board, posed (\d+) of \1 nodes, the pose finite, the board drawn [1-9]\d* times/.test(line));
    if (!await until(() => has(/skate: on/) && statuses().length >= 3, 60)) {
      fail('J did not put the player on the board, posed and drawn' + (has(/skate: on/) ? ' (on, but no good status lines)' : ''));
    }
    // (the skater on the board, standing, the camera behind)
    await shoot('');
    // rolling, a push or two (Space is the board's A), carving (D), then
    // stepped off (Y, the keyboard's Q), the board on the ground: from
    // behind, the skater's boots hide the board
    await page.keyboard.down('KeyW');
    for (let push = 0; push < 3; push++) {
      await press('Space');
      await sleep(800);
    }
    during = await measure();
    await page.keyboard.up('KeyW');
    await shoot('rolling');
    await page.keyboard.down('KeyD');
    await sleep(1000);
    await shoot('carving');
    await page.keyboard.up('KeyD');
    await press('KeyQ');
    await sleep(2500);
    await shoot('stepped-off');
    // off with both sticks clicked in, on the controller
    await page.evaluate(() => window.testPad.sticks(true));
    await sleep(500);
    await page.evaluate(() => window.testPad.sticks(false));
    if (!await until(() => has(/skate: off \(J\)/), 20)) fail('both sticks clicked in did not get off the board');
    if (has(/the pose NOT FINITE/)) fail('a pose was not finite');

    // what the log must show
    const checks = [
      [/halo-skate: preloading the skater from \/data\/skate-data\/assets/, 'the skater preloaded from the imported data'],
      [/halo-skate: preload: session in \d+ms/, 'the preload finished'],
      [/halo-skate: rails: \d+ walkable edges, \d+ lips, \d+ runs, \d+ rails/, "the level's rails"],
      [/halo-skate: board: \d+ vertices, \d+ triangles, \d+ textures/, 'the board loaded'],
      [/halo-skate: sound: \w+ from \/data\/skate-data\/sounds/, 'sounds from the imported folder'],
      [/halo-skate: sound: \d+ of \d+ sounds have samples/, 'the sound set loaded'],
    ];
    for (const [pattern, what] of checks) if (!has(pattern)) fail(`no log line for ${what} (${pattern})`);
    for (const line of [...lines.filter((text) => /halo-skate: sound: \w+ from/.test(text)).slice(0, 2),
      ...statuses().slice(0, 3)]) {
      console.log(`log: ${line}`);
    }
  }
  if (has(/Tried to spawn a new thread, but the thread pool is exhausted/)) fail("the game's thread pool ran out");
  const fps = (value) => (value == null ? 'unknown' : value.toFixed(1));
  const after = withoutData ? 'after J' : 'skating';
  console.log(`frames a second: ${fps(before.fps)} on foot, ${fps(during.fps)} ${after}`);
  console.log(`the browser's memory (proportional set size): ${before.memory.toFixed(0)} MB on foot, ` +
    `${during.memory.toFixed(0)} MB ${after}`);
  if (!process.exitCode) console.log(withoutData ? 'skate in a browser without skate data: ok' : 'skate in a browser: ok');
} catch (error) {
  fail(String(error && error.stack ? error.stack : error));
} finally {
  if (process.env.HALO_SKATE_TEST_LOG) writeFileSync(process.env.HALO_SKATE_TEST_LOG, lines.join('\n') + '\n');
  if (context) await context.close().catch(() => {});
  for (const child of children) child.kill('SIGKILL');
  rmSync(work, { recursive: true, force: true });
}
