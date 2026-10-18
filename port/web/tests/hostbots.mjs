// A browser hosting a game with bots (port/linux/README.md, "Bots"):
//
//     node port/web/tests/hostbots.mjs <site> <maps folder>
//
// (with Playwright where Node finds it; the maps folder holds ui.map and
// bloodgulch.map, which the browser's profile imports). The game is offline
// (network.online false), as skate.mjs's is: hosting online on the CI
// runner, which reaches no broker, the page's own client never finished
// joining its game. The page hosts a system link game on Blood Gulch
// (debug.network_test "host:bloodgulch") with two bots (server.bots): the
// host's bots run in the browser, as the dedicated server's do, so the page
// must log both bots joining, the walking
// graph of Blood Gulch built in the browser (and cached in its private
// storage), and the bots' reports with a walk each, and its own player and
// the bots in the game each second.

import { copyFileSync, mkdirSync, mkdtempSync, rmSync } from 'node:fs';
import { spawn } from 'node:child_process';
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
const sitePort = 8000 + Math.floor(Math.random() * 1000);
const work = mkdtempSync(path.join(tmpdir(), 'opence-hostbots-'));
const children = [];
let context;

function fail(message) {
  console.error('FAILED: ' + message);
  process.exitCode = 1;
}

async function until(test, seconds) {
  for (let waited = 0; waited < seconds; waited++) {
    if (test()) return true;
    await sleep(1000);
  }
  return test();
}

try {
  children.push(spawn('python3', [path.join(root, 'tools/web_serve.py'), site, '--port', String(sitePort)],
    { stdio: 'ignore', env: { ...process.env, WEB_SERVE_QUIET: '1' } }));
  await sleep(1500);
  const pageUrl = `http://localhost:${sitePort}/`;
  const imported = path.join(work, 'maps');
  mkdirSync(imported);
  for (const map of ['ui.map', 'bloodgulch.map']) copyFileSync(path.join(maps, map), path.join(imported, map));
  context = await chromium.launchPersistentContext(path.join(work, 'profile'), {
    headless: true, viewport: { width: 800, height: 450 },
    ...(process.env.HALO_WEBJOIN_CHROMIUM ? { executablePath: process.env.HALO_WEBJOIN_CHROMIUM } : {}),
    args: ['--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist'],
  });
  const page = context.pages()[0] || await context.newPage();
  await page.goto(pageUrl);
  await page.waitForFunction(() => document.querySelectorAll('#checks li').length >= 5, null, { timeout: 60000 });
  await page.setInputFiles('#folder-input', imported);
  await page.waitForFunction(() => /Copied|cannot/.test(document.querySelector('#import-text').textContent), null,
    { timeout: 600000 });
  const lines = [];
  page.on('console', (message) => {
    for (const line of message.text().split('\n')) {
      if (!line) continue;
      lines.push(line);
      if (/bots: |network test: (game|starting)|server: /.test(line)) console.log(`  browser | ${line.slice(0, 180)}`);
    }
  });

  console.log('a browser hosting Blood Gulch with two bots');
  await page.goto(`${pageUrl}?set=HALO_NETWORK_TEST=host:bloodgulch&set=HALO_NETWORK_TEST_START=8` +
    '&set=HALO_SERVER_BOTS=2&set=HALO_BOT_SEED=4&set=HALO_NET_ONLINE=false');
  await page.waitForFunction(() => !document.querySelector('#play').disabled, null, { timeout: 60000 });
  await page.click('#play');
  const joined = () => new Set(lines.map((line) => /bots: (\[BOT\] \w+) joined/.exec(line)).filter(Boolean)
    .map((match) => match[1]));
  const graph = () => lines.find((line) => /bots: walking graph of bloodgulch (built|read from the cache)/.test(line));
  // (each bot's report, every 20 seconds of game: how far it went)
  const walked = () => {
    const went = new Map();
    for (const line of lines) {
      const match = /bots: (\[BOT\] \w+) at tick \d+: .*?went (\d+) world units/.exec(line);
      if (match) went.set(match[1], Number(match[2]));
    }
    return went;
  };
  const botTicks = () => lines.filter((line) => /network test: tick \d+ .* m127/.test(line)).length;
  const done = () => joined().size >= 2 && graph() && walked().size >= 2 &&
    [...walked().values()].every((distance) => distance >= 15) && botTicks() >= 5;
  if (!await until(done, 300)) {
    fail(`the browser's game did not have its two bots play (joined: ${[...joined()].join(', ') || 'none'}; ` +
      `graph: ${graph() ? 'yes' : 'no'}; went: ${JSON.stringify(Object.fromEntries(walked()))}; ` +
      `${botTicks()} seconds with the bot machine's players); the page's last lines:\n` +
      lines.slice(-60).map((line) => `  browser | ${line.slice(0, 240)}`).join('\n'));
  } else {
    console.log(`the browser's bots: ${[...joined()].join(', ')}; ${graph().replace(/.*bots: /, '').slice(0, 120)}`);
    console.log(`they went ${JSON.stringify(Object.fromEntries(walked()))} world units`);
    console.log('a browser hosting bots: ok');
  }
} catch (error) {
  fail(String(error && error.stack ? error.stack : error));
} finally {
  if (context) await context.close().catch(() => {});
  for (const child of children) child.kill('SIGKILL');
  rmSync(work, { recursive: true, force: true });
}
