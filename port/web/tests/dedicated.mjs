// A browser joining a dedicated server (port/linux/README.md, "Dedicated
// server") by its invite, through the server's forwarded address
// (network.public_address):
//
//     node port/web/tests/dedicated.mjs <site> <maps folder> [<halo>]
//
// (with Playwright where Node finds it and mosquitto on the PATH; the maps
// folder holds ui.map and bloodgulch.map, which the browser's profile
// imports). It starts a local broker with a WebSocket listener for the
// browser and a plain one for the server, a dedicated server (<halo>,
// build/linux/halo by default: HALO_DEDICATED, invite-only:
// network.host_public false), and a UDP forward in front of the server's
// tunnel port, as a router's port forward would be, which the server names
// as its network.public_address. The browser opens the site with the
// server's invite (?join=) and must reach the server over WebRTC (the
// server's ICE-lite end and DTLS server, port/linux/src/p2p_webrtc.c)
// through the forward, and play: the server logs the player joining, the
// browser logs its player each second, and the browser's chosen ICE path is
// the forwarded address, which carried the connection's packets.
//
// With HALO_WEBJOIN_INVITE (the server's 64 digits) it starts nothing but the
// browser and joins a server run elsewhere (a container behind a NAT, say),
// through the public brokers or HALO_WEBJOIN_BROKERS (WebSocket URLs, as the
// page's ?brokers= takes them); then
// HALO_WEBJOIN_FORWARDED (host:port) is the address the browser's path must
// use, if given. HALO_WEBJOIN_CHROMIUM names a Chromium to run instead of
// Playwright's own.

import { spawn } from 'node:child_process';
import { copyFileSync, mkdirSync, mkdtempSync, rmSync, symlinkSync, writeFileSync } from 'node:fs';
import { createSocket } from 'node:dgram';
import { createRequire } from 'node:module';
import { networkInterfaces, tmpdir } from 'node:os';
import path from 'node:path';
import { setTimeout as sleep } from 'node:timers/promises';

const require = createRequire(import.meta.url);
const { chromium } = require('playwright');

const here = path.dirname(new URL(import.meta.url).pathname);
const root = path.resolve(here, '../../..');
const site = path.resolve(process.argv[2] || 'build/web/site');
const maps = path.resolve(process.argv[3] || 'assets/maps');
const halo = path.resolve(process.argv[4] || 'build/linux/halo');
const external = process.env.HALO_WEBJOIN_INVITE || '';
const TICKS = 5;
const sitePort = 8000 + Math.floor(Math.random() * 1000);
const tcpPort = 18000 + Math.floor(Math.random() * 1000);
const wsPort = tcpPort + 1000;
const tunnelPort = 27000 + Math.floor(Math.random() * 1000);
const forwardPort = tunnelPort + 1000;
const work = mkdtempSync(path.join(tmpdir(), 'opence-dedicated-'));
const children = [];

function fail(message) {
  console.error('FAILED: ' + message);
  process.exitCode = 1;
}

// this machine's address on its network (not loopback, which a browser's
// ICE may leave out), where the forward listens
function localAddress() {
  for (const addresses of Object.values(networkInterfaces())) {
    for (const address of addresses || []) {
      if (address.family === 'IPv4' && !address.internal) return address.address;
    }
  }
  return '127.0.0.1';
}

// a router's port forward: each source gets its own socket towards the
// server (as a NAT's mapping), and the server's answers go back to it
function forward(listenPort, targetPort) {
  const front = createSocket('udp4');
  const mappings = new Map();
  const counts = { in: 0, out: 0 };
  front.on('message', (message, from) => {
    const key = `${from.address}:${from.port}`;
    let back = mappings.get(key);
    if (!back) {
      back = createSocket('udp4');
      back.on('message', (answer) => {
        counts.out++;
        front.send(answer, from.port, from.address);
      });
      back.bind(0, '127.0.0.1');
      mappings.set(key, back);
    }
    counts.in++;
    back.send(message, targetPort, '127.0.0.1');
  });
  front.bind(listenPort, '0.0.0.0');
  counts.close = () => {
    front.close();
    for (const back of mappings.values()) back.close();
  };
  return counts;
}

function lines(name, sink) {
  return (data) => {
    for (const line of String(data).split('\n')) {
      if (!line) continue;
      sink.push(line);
      if (/Internet play|server: |network test: (joining|tick \d+ )/.test(line)) console.log(`  ${name} | ${line.slice(0, 160)}`);
    }
  };
}

async function until(test, seconds) {
  for (let waited = 0; waited < seconds && !test(); waited++) await sleep(1000);
  return test();
}

let context = null;
let forwarded = null;
try {
  let invite = external;
  let brokers = process.env.HALO_WEBJOIN_BROKERS || '';
  let expectedPath = process.env.HALO_WEBJOIN_FORWARDED || '';
  const serverLines = [];
  if (!external) {
    writeFileSync(path.join(work, 'mosquitto.conf'),
      `listener ${tcpPort} 127.0.0.1\nprotocol mqtt\nlistener ${wsPort} 127.0.0.1\nprotocol websockets\nallow_anonymous true\n`);
    writeFileSync(path.join(work, 'brokers.txt'), `127.0.0.1:${tcpPort}\n`);
    children.push(spawn('mosquitto', ['-c', path.join(work, 'mosquitto.conf')], { stdio: 'ignore' }));
    brokers = `ws://127.0.0.1:${wsPort}`;
    const address = localAddress();
    expectedPath = `${address}:${forwardPort}`;
    forwarded = forward(forwardPort, tunnelPort);
    // the server: its own folders, the maps, no display
    mkdirSync(path.join(work, 'server', 'data'), { recursive: true });
    mkdirSync(path.join(work, 'server', 'saves'));
    symlinkSync(maps, path.join(work, 'server', 'data', 'maps'));
    const env = Object.fromEntries(Object.entries(process.env).filter(([key]) =>
      !/^(HALO_|SDL_|DISPLAY|WAYLAND)/.test(key)));
    Object.assign(env, {
      HALO_DEDICATED: '1', HALO_DATA_ROOT: path.join(work, 'server', 'data'),
      HALO_SAVE_ROOT: path.join(work, 'server', 'saves'), HALO_NET_BROKERS_FILE: path.join(work, 'brokers.txt'),
      HALO_NET_TUNNEL_PORT: String(tunnelPort), HALO_NET_PUBLIC_ADDRESS: expectedPath,
      HALO_NET_HOST_PUBLIC: 'false', HALO_NET_ALLOW_UPNP: 'false', HALO_DISCORD_APPLICATION: '',
      // (no STUN: the browser joins through the forward, on this machine, and
      // the machine's internet address would only end up in the log)
      HALO_NET_STUN: '',
      HALO_UPDATE_ANSWER: 'never', HALO_EXIT_AFTER: '600',
    });
    const server = spawn(halo, [], { cwd: path.join(work, 'server'), env });
    server.stdout.on('data', lines('server', serverLines));
    server.stderr.on('data', lines('server', serverLines));
    children.push(server);
    const found = () => serverLines.map((line) => /halo:\/\/join\/([0-9a-f]{64})/.exec(line)).find(Boolean);
    if (!await until(found, 120)) throw new Error('the dedicated server made no invite');
    invite = found()[1];
    if (!serverLines.some((line) => line.includes(`offering the forwarded address ${expectedPath}`))) {
      throw new Error('the server does not offer its forwarded address');
    }
  }

  // the site, and a browser with the maps imported
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
    // (its local addresses in the clear, as rooms.mjs: no mDNS names, so no multicast
    // from a CI runner whose network policy drops it)
    args: ['--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist',
      '--disable-features=WebRtcHideLocalIpsWithMdns'],
  });
  // (no STUN servers either: the page joins over this machine, and a CI runner's
  // network policy drops STUN, whose answer would be its public address)
  await context.addInitScript(() => {
    const Connection = window.RTCPeerConnection;
    window.RTCPeerConnection = class extends Connection {
      constructor(configuration, ...rest) { super({ ...configuration, iceServers: [] }, ...rest); }
    };
  });
  // (each connection the page makes, so that its chosen path can be read)
  await context.addInitScript(() => {
    const Original = window.RTCPeerConnection;
    if (!Original) return;
    window.__connections = [];
    window.RTCPeerConnection = function (...args) {
      const connection = new Original(...args);
      window.__connections.push(connection);
      return connection;
    };
    window.RTCPeerConnection.prototype = Original.prototype;
    window.RTCPeerConnection.generateCertificate = Original.generateCertificate.bind(Original);
  });
  const page = context.pages()[0] || await context.newPage();
  await page.goto(pageUrl);
  await page.waitForFunction(() => document.querySelectorAll('#checks li').length >= 5, null, { timeout: 60000 });
  await page.setInputFiles('#folder-input', imported);
  await page.waitForFunction(() => /Copied|cannot/.test(document.querySelector('#import-text').textContent), null,
    { timeout: 600000 });
  const browserLines = [];
  page.on('console', (message) => lines('browser', browserLines)(message.text()));

  console.log(`a browser joining the dedicated server by its invite${expectedPath ? `, through ${expectedPath}` : ''}`);
  await page.goto(`${pageUrl}?join=${invite}&set=HALO_NETWORK_TEST=join&set=HALO_NET_JOIN_FROM_CLIPBOARD=0` +
    (brokers ? `&brokers=${brokers}` : ''));
  await page.waitForFunction(() => !document.querySelector('#play').disabled, null, { timeout: 60000 });
  await page.click('#play');
  const ticks = () => browserLines.filter((line) => /network test: tick \d+ .*player 0: \(/.test(line)).length;
  const joined = () => external || serverLines.some((line) => /server: .+ joined/.test(line));
  if (!await until(() => ticks() >= TICKS && joined(), 240)) {
    fail(`the browser did not play on the server (${ticks()} ticks logged` +
      `${external ? '' : joined() ? '; the server saw it join' : '; the server saw nobody join'})`);
  }
  if (!external && !serverLines.some((line) => /WebRTC connected to player .*browser/.test(line))) {
    fail('the server had no WebRTC connection');
  }

  // the browser's chosen path: the forwarded address
  const chosen = await page.evaluate(async () => {
    const paths = [];
    for (const connection of window.__connections || []) {
      if (connection.connectionState !== 'connected') continue;
      const stats = await connection.getStats();
      for (const report of stats.values()) {
        if (report.type !== 'transport' || !report.selectedCandidatePairId) continue;
        const pair = stats.get(report.selectedCandidatePairId);
        const remote = pair && stats.get(pair.remoteCandidateId);
        if (remote) paths.push(`${remote.address || remote.ip}:${remote.port}`);
      }
    }
    return paths;
  });
  console.log(`the browser's WebRTC path: ${chosen.join(', ') || 'none'}`);
  if (expectedPath && !chosen.includes(expectedPath)) fail(`the browser's path is not the forwarded address ${expectedPath}`);
  if (forwarded) {
    console.log(`the forward carried ${forwarded.in} packets in and ${forwarded.out} out`);
    if (forwarded.in < 10 || forwarded.out < 10) fail('the forward carried (almost) nothing');
  }
  if (!process.exitCode) console.log('a browser on the dedicated server: ok');
} catch (error) {
  fail(String(error && error.stack ? error.stack : error));
} finally {
  if (context) await context.close().catch(() => {});
  if (forwarded) forwarded.close();
  for (const child of children) child.kill('SIGKILL');
  rmSync(work, { recursive: true, force: true });
}
