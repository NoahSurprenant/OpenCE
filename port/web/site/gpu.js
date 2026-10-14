/*
GPU.JS

Which GPU the game will draw on, for the launcher's checks (app.js): the
renderer a WebGL 2 context of an OffscreenCanvas in a worker reports (as
the game's own, which draws from its worker), made readable, and whether it
is one to warn about:

- software rendering (SwiftShader, llvmpipe, Microsoft Basic Render Driver):
  the browser has no GPU, or will not use it; the game is very slow;
- an integrated GPU (Intel's but Arc, AMD's "Radeon Graphics" and "Radeon
  NNNM"): slower, and on a computer that also has a dedicated GPU, the
  browser may have been given the integrated one. A page cannot see the
  other GPU, so it only says so.

Neither stops the game: the line is amber, not red.

Chromium gives "ANGLE (vendor, device, backend)":
  ANGLE (NVIDIA, NVIDIA GeForce RTX 3080 (0x00002206) Direct3D11 vs_5_0 ps_5_0, D3D11)
    -> NVIDIA GeForce RTX 3080 (Direct3D 11)
  ANGLE (Intel, Mesa Intel(R) UHD Graphics 620 (KBL GT2), OpenGL 4.6)
    -> Mesa Intel UHD Graphics 620 (KBL GT2) (OpenGL)
  ANGLE (Google, Vulkan 1.3.0 (SwiftShader Device (Subzero) (0x0000C0DE)), SwiftShader driver)
    -> SwiftShader Device (Subzero) (Vulkan)
  ANGLE (Apple, ANGLE Metal Renderer: Apple M2, Unspecified Version)
    -> Apple M2 (Metal)
Firefox gives the device alone, already tidied.
*/

'use strict';

(function (exports) {
  const SOFTWARE = /SwiftShader|llvmpipe|softpipe|lavapipe|Software Rasterizer|Basic Render Driver|Software Adapter/i;
  const INTEGRATED = /\bIntel\b(?!.*\bArc\b)|\bRadeon(\(TM\))?( Vega \d+)? Graphics\b|\bRadeon(\(TM\))? \d{3}M\b/i;

  // a list's parts at its top-level commas (not those within parentheses)
  function topLevelParts(text) {
    const parts = [];
    let depth = 0;
    let start = 0;
    for (let index = 0; index < text.length; index++) {
      const character = text[index];
      if (character === '(') depth++;
      else if (character === ')') depth = Math.max(0, depth - 1);
      else if (character === ',' && depth === 0) {
        parts.push(text.slice(start, index).trim());
        start = index + 1;
      }
    }
    parts.push(text.slice(start).trim());
    return parts;
  }

  function backendName(backend) {
    let match;
    if ((match = /^D3D(\d+)/i.exec(backend))) return 'Direct3D ' + match[1];
    if (/^OpenGL ES/i.test(backend)) return 'OpenGL ES';
    if (/^OpenGL/i.test(backend)) return 'OpenGL';
    if (/^Vulkan/i.test(backend)) return 'Vulkan';
    if (/^Metal/i.test(backend)) return 'Metal';
    return '';
  }

  // a device's name without what only a driver needs: PCI ids, shader
  // models, (R) and (TM)
  function tidy(device) {
    return device
      .replace(/\s*\(0x[0-9a-f]+\)/gi, '')
      .replace(/\s+Direct3D\s*\d+\S*(\s+[vp]s_\d_\d)*/gi, '')
      .replace(/\/PCIe\/SSE2|\/PCI\/SSE2/gi, '')
      .replace(/\((R|TM)\)/gi, '')
      .replace(/\s+/g, ' ')
      .trim();
  }

  // { name, state ('good', 'warn', or 'unknown': the checks' grey), kind
  // ('hardware', 'integrated', 'software' or 'unknown'), raw } of a WebGL
  // renderer string
  function describe(renderer) {
    const raw = typeof renderer === 'string' ? renderer.trim() : '';
    if (!raw || /^(WebKit WebGL|Mozilla|WebKit)$/i.test(raw)) {
      return { name: 'not reported by this browser', state: 'unknown', kind: 'unknown', raw };
    }
    let device = raw;
    let backend = '';
    const angle = /^ANGLE \((.*)\)$/.exec(raw);
    if (angle) {
      const parts = topLevelParts(angle[1]);
      device = parts.length >= 2 ? parts[1] : parts[0];
      backend = backendName(parts[2] || '');
      let match;
      if ((match = /^Vulkan [\d.]+ \((.*)\)$/i.exec(device))) {
        device = match[1];
        backend = 'Vulkan';
      } else if ((match = /^ANGLE Metal Renderer: (.*)$/i.exec(device))) {
        device = match[1];
        backend = 'Metal';
      }
    }
    // (Firefox's, and ANGLE's on Direct3D: "... Direct3D11 vs_5_0 ps_5_0")
    const direct3d = /\bDirect3D\s*(\d+)/i.exec(device);
    if (!backend && direct3d) backend = 'Direct3D ' + direct3d[1];
    const name = tidy(device) + (backend ? ` (${backend})` : '');
    if (SOFTWARE.test(raw)) return { name, state: 'warn', kind: 'software', raw };
    if (INTEGRATED.test(device)) return { name, state: 'warn', kind: 'integrated', raw };
    return { name, state: 'good', kind: 'hardware', raw };
  }

  // what to do about it, for the line's detail
  function hint(kind) {
    if (kind === 'software') {
      return '(software rendering: the game will be slow) Turn on graphics acceleration in the browser\'s ' +
        'settings; on Windows, set the browser to High performance in Settings > System > Display > Graphics. ' +
        'chrome://gpu (about:support in Firefox) shows what it uses.';
    }
    if (kind === 'integrated') {
      return '(an integrated GPU) If this computer also has a dedicated one, on Windows set the browser to ' +
        'High performance in Settings > System > Display > Graphics. chrome://gpu (about:support in Firefox) ' +
        'shows what it uses.';
    }
    return '';
  }

  Object.assign(exports, { describe, hint });
})(typeof module !== 'undefined' && module.exports ? module.exports : (self.HaloGpu = {}));
