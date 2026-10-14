// The site in a headless Chromium, as GitHub Pages serves it (without the
// isolation headers, which the service worker then adds):
//
//     node port/web/tests/smoke.mjs dist/halo-web-release
//     node port/web/tests/smoke.mjs dist/halo-web-release --base /OpenCE/
//
// (--base: under a sub-path, as GitHub Pages serves a repository's site
// without a domain of its own, https://<owner>.github.io/<repository>/; the
// service worker's scope must then be that sub-path)
//
// (with Playwright installed where Node finds it: .github/workflows/build.yml).
// It checks that the page isolates itself after one reload, that every check
// passes (threads, WebGL 2 in a worker, OPFS, the memory), that it reports no
// game data, and that the game starts: with an empty maps folder marked
// complete, the WebAssembly is compiled, its 2176 MiB memory made, its threads
// started, the browser's storage mounted and WebGL 2 set up, and it looks for
// the menus' map, which it reports it cannot find.

import { spawn } from 'node:child_process';
import { setTimeout as sleep } from 'node:timers/promises';
import { createRequire } from 'node:module';
import path from 'node:path';

const require = createRequire(import.meta.url);
const { chromium } = require('playwright');

const args = process.argv.slice(2);
const baseIndex = args.indexOf('--base');
const base = baseIndex >= 0 ? args.splice(baseIndex, 2)[1] : '/';
const site = path.resolve(args[0] || 'build/web/site');
const port = 8000 + Math.floor(Math.random() * 1000);
const server = spawn('python3', [path.join(path.dirname(new URL(import.meta.url).pathname), '../../../tools/web_serve.py'),
  site, '--port', String(port), '--as-github-pages', '--base', base], { stdio: 'inherit', env: { ...process.env, WEB_SERVE_QUIET: '1' } });
const url = new URL(base.replace(/^\/*/, '/').replace(/\/*$/, '/'), `http://localhost:${port}`).href;

function fail(message) {
  console.error('FAILED: ' + message);
  process.exitCode = 1;
}

let browser;
const log = [];
try {
  await sleep(1500);
  browser = await chromium.launch({
    args: ['--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist'],
  });
  const page = await browser.newPage();
  page.on('console', (message) => log.push(message.text()));
  page.on('pageerror', (error) => log.push('page error: ' + error));

  await page.goto(url);
  await page.waitForFunction(() => window.crossOriginIsolated && document.querySelectorAll('#checks li').length >= 5,
    null, { timeout: 60000 });
  const scope = await page.evaluate(async () => (await navigator.serviceWorker.getRegistration()).scope);
  console.log('service worker: ' + scope);
  if (scope !== url) fail(`the service worker's scope is ${scope}, not ${url}`);
  // the web app manifest, as the browser fetched it (with the cookies, under
  // the service worker: index.html)
  const cdp = await page.context().newCDPSession(page);
  const manifest = await cdp.send('Page.getAppManifest');
  console.log(`manifest: ${manifest.url} (${manifest.data ? manifest.data.length + ' bytes' : 'none'})`);
  if (manifest.url !== url + 'manifest.webmanifest') fail(`the manifest is ${manifest.url}, not ${url}manifest.webmanifest`);
  if (!manifest.data || !/"name": "OpenCE"/.test(manifest.data)) fail('the browser did not get the manifest');
  if (manifest.errors.length) fail('the manifest has errors: ' + JSON.stringify(manifest.errors));
  const checks = await page.$$eval('#checks li', (items) => items.map((item) => [item.className, item.textContent]));
  console.log(checks.map(([state, text]) => `${state}: ${text}`).join('\n'));
  if (checks.some(([state]) => state !== 'good')) fail('a check did not pass');
  await page.waitForFunction(() => !/Looking/.test(document.querySelector('#data-status').textContent));
  const data = await page.textContent('#data-status');
  console.log('data: ' + data);
  if (!/No game data/.test(data)) fail('the page should have no game data');
  if (!(await page.isDisabled('#play'))) fail('Play should wait for the game data');

  // an empty maps folder, marked complete: the game starts and looks for a
  // map
  await page.evaluate(async () => {
    const root = await navigator.storage.getDirectory();
    const maps = await root.getDirectoryHandle('maps', { create: true });
    const marker = await (await maps.getFileHandle('.complete', { create: true })).createWritable();
    await marker.write(JSON.stringify({ files: [], bytes: 0 }));
    await marker.close();
  });
  await page.reload();
  await page.waitForFunction(() => !document.querySelector('#play').disabled, null, { timeout: 60000 });
  await page.click('#play');
  const started = Date.now();
  while (!log.some((line) => /couldn't find map 'ui'/.test(line)) && Date.now() - started < 120000) await sleep(500);
  if (!log.some((line) => /data root: \/data/.test(line))) fail('the game did not mount the storage at /data');
  if (!log.some((line) => /OpenGL ES 3\.0 \(WebGL 2/.test(line))) fail('the game did not set up WebGL 2');
  if (!log.some((line) => /couldn't find map 'ui'/.test(line))) fail('the game did not get as far as the maps');
  if (!(await page.isHidden('#fatal'))) fail('the game stopped: ' + (await page.textContent('#fatal-text')));
  console.log('the game started and looked for its maps');
} catch (error) {
  fail(String(error && error.stack ? error.stack : error));
} finally {
  if (process.exitCode) console.error('the page logged:\n' + log.slice(-40).join('\n'));
  if (browser) await browser.close();
  server.kill();
}
