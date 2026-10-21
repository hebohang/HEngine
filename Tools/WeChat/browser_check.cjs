// Auxiliary browser runtime evidence; never labelled as WeChat validation.
const fs = require('node:fs');
const path = require('node:path');
const http = require('node:http');
const assert = require('node:assert/strict');
const { chromium } = require('playwright');
const { PNG } = require('pngjs');

async function main() {
  const root = path.resolve(process.argv[2] || 'out/agent/wechat-export');
  const evidence = path.resolve(process.argv[3] || 'out/agent/wechat/evidence/browser');
  fs.mkdirSync(evidence, { recursive: true });
  const server = http.createServer((request, response) => {
    const file = path.resolve(root, '.' + decodeURIComponent(new URL(request.url, 'http://localhost').pathname));
    if (!file.startsWith(root + path.sep) || !fs.existsSync(file) || !fs.statSync(file).isFile()) { response.writeHead(404).end(); return; }
    const types = { '.wasm': 'application/wasm', '.js': 'text/javascript', '.html': 'text/html', '.png': 'image/png', '.wav': 'audio/wav' };
    response.setHeader('Content-Type', types[path.extname(file)] || 'application/octet-stream');
    fs.createReadStream(file).pipe(response);
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  let browser;
  const log = [];
  try {
    browser = await chromium.launch({ headless: process.env.HE_WECHAT_HEADED !== '1', executablePath: process.env.HE_WECHAT_CHROME || 'C:/Program Files/Google/Chrome/Application/chrome.exe' });
    const page = await browser.newPage({ viewport: { width: 480, height: 720 }, deviceScaleFactor: 1 });
    // Model the Android host's unsupported CURRENT_PROGRAM query. The complete
    // draw/reload suite must work without querying it or suppressing GL errors.
    await page.addInitScript(() => {
      const original = WebGL2RenderingContext.prototype.getParameter;
      WebGL2RenderingContext.prototype.getParameter = function(name) {
        if (name === 0x8B8D) throw new Error('CURRENT_PROGRAM host query is unavailable');
        return original.call(this, name);
      };
    });
    page.on('console', event => { log.push(`${event.type()}: ${event.text()}`); });
    page.on('pageerror', error => { log.push(`PAGE ERROR: ${error.stack}`); });
    await page.goto(`http://127.0.0.1:${server.address().port}/browser-test.html`);
    await page.waitForFunction(() => window.HEBrowser && (HEBrowser.session.isReady() || HEBrowser.session.getMetrics().errors.length), null, { timeout: 30000 });
    assert.deepEqual(await page.evaluate(() => HEBrowser.session.getMetrics().errors), [], 'startup diagnostics');
    await page.waitForFunction(() => HEBrowser.session.getMetrics().frames >= 5);
    const bootRecord = await page.evaluate(() => JSON.parse(localStorage.getItem('he-wechat-last-boot')));
    assert.equal(bootRecord.stage, 'ready'); assert(bootRecord.done);
    assert.equal(bootRecord.buildId, await page.evaluate(() => HEBrowser.config.buildId));
    assert(bootRecord.history.findIndex(item => item.stage === 'bootstrap-presented') < bootRecord.history.findIndex(item => item.stage === 'wasm'));
    const first = await page.locator('canvas').screenshot({ path: path.join(evidence, 'initial.png') });
    const pixels = PNG.sync.read(first);
    const colors = new Set();
    // Exclude the diagnostic text overlay: a clear-only canvas must fail.
    for (let y = 64; y < pixels.height; y++)
      for (let x = 0; x < pixels.width; x++) colors.add(pixels.data.readUInt32BE((y * pixels.width + x) * 4));
    assert(colors.size >= 3, 'clear-only canvas: texture/color draws missing');
    const hasColor = expected => [...colors].some(value => expected.every((channel, i) => Math.abs(((value >>> (24 - i * 8)) & 255) - channel) <= 2));
    assert(hasColor([53, 140, 177]) && hasColor([16, 48, 82]), 'linear texture/tint/straight-alpha blend against clear color');
    const sample = (x, y) => [...pixels.data.subarray((y * pixels.width + x) * 4, (y * pixels.width + x) * 4 + 3)];
    for (const [actual, expected] of [[sample(160, 280), [53, 140, 177]], [sample(205, 280), [16, 48, 82]]])
      assert(actual.every((value, i) => Math.abs(value - expected[i]) <= 2), 'UV origin or transformed screen placement mismatch');
    await page.mouse.move(360, 240); await page.mouse.down(); await page.waitForTimeout(250); await page.mouse.up();
    await page.evaluate(() => HEBrowser.session.save());
    await page.waitForFunction(() => HEBrowser.session.getMetrics().audioEndedEvents >= 1);
    const saved = await page.evaluate(() => JSON.parse(localStorage.getItem(HEBrowser.config.storageKey)));
    assert.equal(saved.script.Properties.Score, 1);
    assert(saved.transform.Translation[0] > 0 && saved.transform.Translation[1] > 0, 'transformed C++ movement');
    const moved = await page.locator('canvas').screenshot({ path: path.join(evidence, 'moved.png') });
    assert(!first.equals(moved), 'visible state did not change');
    await page.evaluate(() => HEBrowser.fire('Hide'));
    const frameCount = await page.evaluate(() => HEBrowser.session.getMetrics().frames);
    await page.waitForTimeout(120);
    assert.equal(await page.evaluate(() => HEBrowser.session.getMetrics().frames), frameCount);
    await page.evaluate(() => HEBrowser.fire('Show'));
    await page.waitForFunction(count => HEBrowser.session.getMetrics().frames > count, frameCount);
    await page.evaluate(() => HEBrowser.session.save());
    assert.deepEqual(await page.evaluate(() => JSON.parse(localStorage.getItem(HEBrowser.config.storageKey)).transform), saved.transform);
    const metrics = await page.evaluate(() => HEBrowser.session.getMetrics());
    await page.reload();
    await page.waitForFunction(() => window.HEBrowser && HEBrowser.session.isReady());
    await page.evaluate(() => HEBrowser.session.save());
    assert.deepEqual(await page.evaluate(() => JSON.parse(localStorage.getItem(HEBrowser.config.storageKey))), saved);
    await page.setViewportSize({ width: 720, height: 480 });
    await page.waitForTimeout(100);
    assert.equal(await page.locator('canvas').evaluate(canvas => canvas.width), 720);
    assert.equal((await page.evaluate(() => HEBrowser.session.getMetrics())).errors.length, 0);
    await page.evaluate(() => HEBrowser.session.shutdown());
    const graphics = await page.locator('canvas').evaluate(canvas => {
      const gl = canvas.getContext('webgl2'), extension = gl.getExtension('WEBGL_debug_renderer_info');
      return extension ? gl.getParameter(extension.UNMASKED_RENDERER_WEBGL) : 'unavailable';
    });
    const faultChecks = [];
    for (const fault of [
      { method: 'bufferData', operation: '_sg_gl_create_buffer', onCall: 1, frames: 0 },
      { method: 'clearBufferfi', operation: '_sg_gl_begin_pass', onCall: 1, frames: 0 },
      { method: 'uniform4fv', operation: 'sg_apply_uniforms', onCall: 1, frames: 0 },
      { method: 'drawArrays', operation: 'sg_draw', onCall: 1, frames: 0 },
      { method: 'drawArrays', operation: 'frame submission', onCall: 3, frames: 2 },
      // After the two bootstrap clears and context registration, simulate a
      // persistently broken driver. Startup must fail, never spin draining it.
      { method: 'getError', operation: 'Sokol setup', onCall: 4, frames: 0, persistent: true }
    ]) {
      const faultContext = await browser.newContext({ viewport: { width: 480, height: 720 } });
      try {
        await faultContext.addInitScript(({ method, onCall, persistent }) => {
          const original = WebGL2RenderingContext.prototype[method];
          let calls = 0;
          WebGL2RenderingContext.prototype[method] = function(...args) {
            if (persistent && ++calls >= onCall) { window.HEInjectedGraphicsFault = method; return 0x500; }
            const result = original.apply(this, args);
            // Generate a real GL error, without mocking getError or HE's checks.
            if (!persistent && ++calls === onCall) { this.enable(0xdead); window.HEInjectedGraphicsFault = method; }
            return result;
          };
        }, fault);
        const faultPage = await faultContext.newPage();
        await faultPage.goto(page.url());
        await faultPage.waitForFunction(() => window.HEBrowser?.session.getMetrics().errors.length > 0, null, { timeout: 10000 });
        const result = await faultPage.evaluate(() => ({ injected: window.HEInjectedGraphicsFault,
          ready: HEBrowser.session.isReady(), metrics: HEBrowser.session.getMetrics(),
          failure: JSON.parse(localStorage.getItem('he-wechat-last-boot-error')) }));
        assert.equal(result.injected, fault.method);
        assert.equal(result.ready, false, 'GL failure must stop startup/rendering');
        assert.equal(result.metrics.frames, fault.frames);
        assert.match(result.metrics.errors[0], /GL_INVALID_ENUM \(1280\)/);
        assert(result.metrics.errors[0].includes(fault.operation), result.metrics.errors[0]);
        assert.match(result.failure.message, /GL_INVALID_ENUM \(1280\)/);
        faultChecks.push({ ...fault, error: result.metrics.errors[0] });
      } finally { await faultContext.close(); }
    }
    const packageManifest = path.join(root,'game-manifest.json');
    fs.writeFileSync(path.join(evidence, 'result.json'), JSON.stringify({ status: 'PASS', evidence: 'auxiliary-browser', headed: process.env.HE_WECHAT_HEADED === '1',
      visualReview: 'PENDING', packageBuildId: fs.existsSync(packageManifest) ? JSON.parse(fs.readFileSync(packageManifest)).buildId : null,
      browser: browser.version(), os: process.platform, graphics,
      viewport: [480, 720], currentProgramQueryUnavailable: true, uniqueCanvasColors: colors.size, metrics, bootRecord, faultChecks,
      checks: ['real Wasm C++ execution', 'Sokol GLES3 shader compile/link/draw', 'texture/color/transform/alpha path', 'C++ touch state', 'persistent storage reload', 'hide/show no held input', 'resize', 'real GL faults preserve code, location and stop rendering'] }, null, 2));
    console.log('PASS auxiliary browser runtime; evidence: ' + evidence);
  } finally {
    fs.writeFileSync(path.join(evidence, 'console.log'), log.join('\n'));
    if (browser) await browser.close();
    await new Promise(resolve => server.close(resolve));
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
