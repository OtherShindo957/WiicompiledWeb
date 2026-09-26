// Run with a separately installed Playwright (see docs/web-probe.md).
const {chromium} = require('playwright');
const assert = require('node:assert/strict');

(async () => {
  const browser = await chromium.launch({
    executablePath: process.env.CHROMIUM_PATH || undefined,
    headless: true,
    args: process.env.WEBGPU_SOFTWARE === '1'
      ? ['--enable-unsafe-webgpu', '--use-angle=swiftshader', '--enable-features=Vulkan', '--use-vulkan=swiftshader']
      : [],
  });
  try {
    const base = process.env.PROBE_URL || 'http://localhost:8000';
    const page = await browser.newPage();
    const errors = [];
    page.on('pageerror', error => errors.push(error.message));
    const response = await page.goto(base);
    assert.equal(response.headers()['cross-origin-opener-policy'], 'same-origin');
    assert.equal(response.headers()['cross-origin-embedder-policy'], 'require-corp');
    assert.equal(await page.evaluate(() => crossOriginIsolated), true);
    await page.getByRole('button', {name: 'Click to Start'}).click();
    await page.waitForFunction(() => /Running|Failed/.test(document.querySelector('#status').textContent));
    const log = await page.locator('#log').innerText();
    console.log(log);
    assert.match(log, /WASM main entered/);
    assert.match(log, /WebGPU canvas clear submitted and completed/);
    assert.match(log, /WiiCompiled Web runtime initialized/);
    assert.doesNotMatch(log, /\[ERROR\]/);
    await page.setViewportSize({width: 640, height: 700});
    await page.waitForFunction(() => {
      const c = document.querySelector('canvas');
      return c.width === Math.round(c.clientWidth * devicePixelRatio);
    });
    // Read rendered canvas pixels through a separate 2D canvas, not a CSS screenshot.
    // A fresh RAF observes presentation from the native main-loop callback.
    const rgba = await page.evaluate(() => new Promise(resolve => requestAnimationFrame(() => {
      const source = document.querySelector('canvas');
      const copy = document.createElement('canvas');
      copy.width = source.width; copy.height = source.height;
      const context = copy.getContext('2d');
      context.drawImage(source, 0, 0);
      resolve([...context.getImageData(copy.width >> 1, copy.height >> 1, 1, 1).data]);
    })));
    console.log('Canvas center RGBA:', rgba);
    assert.ok(Math.abs(rgba[0] - 6) <= 2 && Math.abs(rgba[1] - 41) <= 2 && Math.abs(rgba[2] - 61) <= 2 && rgba[3] === 255,
      `Unexpected clear pixel: ${rgba}`);
    assert.deepEqual(errors, []);
    if (process.env.PROBE_SCREENSHOT) await page.screenshot({path: process.env.PROBE_SCREENSHOT});
    await page.close();

    for (const scenario of ['missing', 'null-adapter', 'device-failure', 'device-loss', 'missing-wasm']) {
      const p = await browser.newPage();
      if (scenario === 'missing-wasm') {
        await p.route('**/wiicompiled-web.wasm', route => route.abort());
      } else {
        await p.addInitScript(scenario => {
          if (scenario === 'missing') {
            Object.defineProperty(navigator, 'gpu', {value: undefined});
          } else if (scenario === 'null-adapter') {
            Object.defineProperty(navigator, 'gpu', {value: {requestAdapter: async () => null}});
          } else if (scenario === 'device-failure') {
            Object.defineProperty(navigator, 'gpu', {value: {requestAdapter: async () => ({requestDevice: async () => { throw new Error('Synthetic device failure'); }})}});
          } else {
            const original = navigator.gpu.requestAdapter.bind(navigator.gpu);
            navigator.gpu.requestAdapter = async (...args) => {
              const adapter = await original(...args);
              const request = adapter.requestDevice.bind(adapter);
              adapter.requestDevice = async (...options) => {
                const device = await request(...options);
                globalThis.destroyProbeDevice = () => device.destroy();
                return device;
              };
              return adapter;
            };
          }
        }, scenario);
      }
      await p.goto(base);
      await p.getByRole('button', {name: 'Click to Start'}).click();
      if (scenario === 'device-loss') {
        await p.waitForFunction(() => document.querySelector('#status').textContent.startsWith('Running'));
        await p.evaluate(() => destroyProbeDevice());
      }
      await p.waitForFunction(() => document.querySelector('#status').textContent.startsWith('Failed'));
      const failureLog = await p.locator('#log').innerText();
      assert.match(failureLog, /\[ERROR\]/);
      if (scenario !== 'device-loss') assert.doesNotMatch(failureLog, /WiiCompiled Web runtime initialized/);
      console.log(`PASS ${scenario}`);
      await p.close();
    }
    console.log('PASS real WASM, GPU clear/pixel, resize, isolation and failure paths');
  } finally {
    await browser.close();
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
