// The launcher's GPU line (port/web/site/gpu.js): renderer strings as
// browsers report them, made readable, and which ones it warns about.
// node --test "port/web/tests/*.test.js"

'use strict';

const test = require('node:test');
const assert = require('node:assert');
const gpu = require('../site/gpu.js');

const cases = [
  // Chromium on Windows (ANGLE on Direct3D 11)
  ['ANGLE (NVIDIA, NVIDIA GeForce RTX 3080 (0x00002206) Direct3D11 vs_5_0 ps_5_0, D3D11)',
    'NVIDIA GeForce RTX 3080 (Direct3D 11)', 'good', 'hardware'],
  ['ANGLE (AMD, AMD Radeon RX 6800 XT Direct3D11 vs_5_0 ps_5_0, D3D11)',
    'AMD Radeon RX 6800 XT (Direct3D 11)', 'good', 'hardware'],
  ['ANGLE (Intel, Intel(R) UHD Graphics 620 (0x00005917) Direct3D11 vs_5_0 ps_5_0, D3D11-27.20.100.8681)',
    'Intel UHD Graphics 620 (Direct3D 11)', 'warn', 'integrated'],
  ['ANGLE (Intel, Intel(R) Arc(TM) A770 Graphics (0x000056A0) Direct3D11 vs_5_0 ps_5_0, D3D11)',
    'Intel Arc A770 Graphics (Direct3D 11)', 'good', 'hardware'],
  ['ANGLE (AMD, AMD Radeon(TM) Graphics (0x00001681) Direct3D11 vs_5_0 ps_5_0, D3D11)',
    'AMD Radeon Graphics (Direct3D 11)', 'warn', 'integrated'],
  ['ANGLE (AMD, AMD Radeon 780M Graphics (0x000015BF) Direct3D11 vs_5_0 ps_5_0, D3D11)',
    'AMD Radeon 780M Graphics (Direct3D 11)', 'warn', 'integrated'],
  ['ANGLE (Microsoft, Microsoft Basic Render Driver (0x0000008C) Direct3D11 vs_5_0 ps_5_0, D3D11)',
    'Microsoft Basic Render Driver (Direct3D 11)', 'warn', 'software'],
  // Chromium on Linux (ANGLE on OpenGL, or Vulkan), on a Mac (Metal)
  ['ANGLE (NVIDIA Corporation, NVIDIA GeForce GTX 1080/PCIe/SSE2, OpenGL 4.5.0 NVIDIA 535.183.01)',
    'NVIDIA GeForce GTX 1080 (OpenGL)', 'good', 'hardware'],
  ['ANGLE (Intel, Mesa Intel(R) UHD Graphics 620 (KBL GT2), OpenGL 4.6)',
    'Mesa Intel UHD Graphics 620 (KBL GT2) (OpenGL)', 'warn', 'integrated'],
  ['ANGLE (Mesa, llvmpipe (LLVM 15.0.7, 256 bits), OpenGL 4.5)', 'llvmpipe (LLVM 15.0.7, 256 bits) (OpenGL)', 'warn', 'software'],
  ['ANGLE (Google, Vulkan 1.3.0 (SwiftShader Device (Subzero) (0x0000C0DE)), SwiftShader driver)',
    'SwiftShader Device (Subzero) (Vulkan)', 'warn', 'software'],
  ['ANGLE (Apple, ANGLE Metal Renderer: Apple M2, Unspecified Version)', 'Apple M2 (Metal)', 'good', 'hardware'],
  // Firefox (the device alone)
  ['NVIDIA GeForce GTX 980 Direct3D11 vs_5_0 ps_5_0, or similar', 'NVIDIA GeForce GTX 980, or similar (Direct3D 11)', 'good', 'hardware'],
  ['Mesa Intel(R) UHD Graphics 620 (KBL GT2)', 'Mesa Intel UHD Graphics 620 (KBL GT2)', 'warn', 'integrated'],
  // not reported
  ['WebKit WebGL', 'not reported by this browser', 'unknown', 'unknown'],
  [null, 'not reported by this browser', 'unknown', 'unknown'],
];

for (const [renderer, name, state, kind] of cases) {
  test(`${renderer}`, () => {
    const described = gpu.describe(renderer);
    assert.deepStrictEqual({ name: described.name, state: described.state, kind: described.kind }, { name, state, kind });
  });
}

test('a warning comes with what to do, a good GPU with nothing', () => {
  assert.match(gpu.hint('software'), /software rendering.*High performance.*chrome:\/\/gpu/);
  assert.match(gpu.hint('integrated'), /integrated GPU.*High performance.*chrome:\/\/gpu/);
  assert.strictEqual(gpu.hint('hardware'), '');
});
