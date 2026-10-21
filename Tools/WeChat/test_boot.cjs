'use strict';
const assert = require('node:assert/strict');
const { createBootDiagnostics } = require('../../Engine/Source/Runtime/Platform/WeChat/boot-diagnostics.js');
const originals = Object.fromEntries(['setTimeout', 'clearTimeout', 'requestAnimationFrame', 'cancelAnimationFrame'].map(key => [key, global[key]]));
const timers = new Map(), frames = new Map();
let sequence = 0, clock = 0;
global.setTimeout = (fn, delay) => { timers.set(++sequence, { fn, due: clock + delay }); return sequence; };
global.clearTimeout = id => timers.delete(id);
global.requestAnimationFrame = fn => { frames.set(++sequence, fn); return sequence; };
global.cancelAnimationFrame = id => frames.delete(id);
function frame() { const batch = [...frames.values()]; frames.clear(); batch.forEach(fn => fn(clock)); }
function advance(ms) { clock += ms; for (const [id, timer] of [...timers]) if (timer.due <= clock) { timers.delete(id); timer.fn(); } }

(async () => {
  try {
    const records = new Map(), modals = [], stages = [], debug = [];
    let hides = 0, clears = 0;
    const wx = { showLoading: value => stages.push(value.title), hideLoading: () => hides++,
      setStorageSync: (key, value) => records.set(key, value), showModal: value => modals.push(value),
      setEnableDebug: value => debug.push(value.enableDebug) };
    const context = { viewport() {}, clearColor() {}, clear() { clears++; }, getError: () => 0, COLOR_BUFFER_BIT: 0x4000 };
    const canvas = { width: 200, height: 400 };
    const ready = createBootDiagnostics(wx);
    ready.identify('he-test-build'); assert.deepEqual(debug, [], 'debug opt-in only');
    ready.identify('he-test-build', true); assert.deepEqual(debug, [true]);
    let presented = false;
    const presentation = ready.present(canvas, context).then(() => { presented = true; });
    frame(); await Promise.resolve(); assert(!presented, 'yield the painted frame before C++ may block');
    frame(); await presentation;
    assert.equal(clears, 2); assert.equal(records.get('he-wechat-last-boot').stage, 'bootstrap-presented');
    ready.stage('first-frame'); ready.ready();
    assert.equal(timers.size, 0); assert.equal(frames.size, 0); assert.equal(hides, 1); assert.equal(modals.length, 0);
    assert.equal(records.get('he-wechat-last-boot').buildId, 'he-test-build');
    assert.equal(records.get('he-wechat-last-boot').stage, 'ready');

    const failure = createBootDiagnostics(wx);
    failure.stage('wasm'); advance(30000); failure.fail(new Error('secondary failure'));
    assert.equal(modals.length, 1); assert.equal(records.get('he-wechat-last-boot-error').stage, 'wasm');
    assert.match(modals[0].content, /30 seconds/); assert.equal(timers.size, 0);

    const unavailableFrames = createBootDiagnostics(wx);
    const stalled = assert.rejects(unavailableFrames.present(canvas, context), /scheduling timed out/);
    advance(2000); await stalled;
    unavailableFrames.fail(new Error('No animation frames'));
    assert.equal(timers.size, 0); assert.equal(frames.size, 0);

    const cancelled = createBootDiagnostics(wx);
    const pending = assert.rejects(cancelled.present(canvas, context), /cancelled/);
    cancelled.fail(new Error('cancelled')); await pending;
    assert.equal(timers.size, 0); assert.equal(frames.size, 0);

    const badCanvas = createBootDiagnostics(wx);
    await assert.rejects(badCanvas.present(canvas, { ...context, getError: () => 1280 }), /Bootstrap canvas GL error: 1280/);
    badCanvas.fail(new Error('canvas error')); assert.equal(timers.size, 0);

    const unavailableUI = createBootDiagnostics({});
    unavailableUI.fail(new Error('original startup failure'));
    assert.equal(timers.size, 0, 'missing native UI must not mask failure or leak timers');
    console.log('PASS boot diagnostics: presented frame, persistent build/stages, debug opt-in, timeout, cancellation, GL/UI failures');
  } finally {
    for (const [key, value] of Object.entries(originals)) {
      if (value === undefined) delete global[key]; else global[key] = value;
    }
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
