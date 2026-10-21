'use strict';
const assert = require('node:assert/strict');
const { createHost } = require('../../Engine/Source/Runtime/Platform/WeChat/host.js');
const callbacks = new Map();
let scheduled = new Map(), sequence = 0;
global.requestAnimationFrame = fn => { scheduled.set(++sequence, fn); return sequence; };
global.cancelAnimationFrame = id => scheduled.delete(id);
const frame = time => { const batch = [...scheduled.values()]; scheduled.clear(); batch.forEach(fn => fn(time)); };

function fixture(options = {}) {
  callbacks.clear(); scheduled.clear();
  const calls = { delta: [], touches: [], audio: 0, saved: [], initSaved: [], shutdown: 0, reads: [] };
  let saved = options.saved, playListener;
  const wx = {
    createCanvas: () => ({ getContext: () => options.noGL ? null : { createVertexArray() {}, texStorage2D() {}, getParameter: () => 'mock' } }),
    getWindowInfo: () => ({ windowWidth: 200, windowHeight: 400, pixelRatio: 2 }),
    getFileSystemManager: () => ({ readFile: ({ filePath, success, fail }) => { calls.reads.push(filePath); if (options.readHang) return; return options.readError ? fail({ errMsg: 'asset missing' }) : success({ data: new ArrayBuffer(options.wrongSize ? 2 : 1) }); } }),
    createInnerAudioContext: () => ({ play() { calls.audio++; playListener?.(); }, pause() {}, stop() {}, destroy() {}, seek() {}, onPlay(fn) { playListener = fn; }, onEnded() {}, onError() {} }),
    getStorageInfo: ({ success, fail }) => options.storageReadError ? fail({ errMsg: 'storage permission denied' }) : success({ keys: saved === undefined ? [] : ['save'] }),
    getStorage: ({ success }) => success({ data: saved }),
    setStorage: ({ data, success, fail }) => { if (options.saveError) return fail({ errMsg: 'storage full' }); saved = data; calls.saved.push(data); success({}); }
  };
  for (const event of ['TouchStart', 'TouchMove', 'TouchEnd', 'TouchCancel', 'Show', 'Hide', 'WindowResize']) {
    wx['on' + event] = fn => callbacks.set(event, fn); wx['off' + event] = () => callbacks.delete(event);
  }
  const module = {
    FS: { mkdirTree() {}, writeFile() {} }, stringToNewUTF8: value => value, UTF8ToString: value => value, _free() {},
    _he_init(root, scene, texture, save) { calls.initSaved.push(save); return 1; },
    _he_shutdown() { calls.shutdown++; return 1; }, _he_last_error: () => 'C++ error', _he_resize: () => 1,
    _he_touch: (...values) => calls.touches.push(values), _he_visible: () => 1, _he_frame(delta) { calls.delta.push(delta); return 1; },
    _he_save: () => '{"schema":1}', _he_heap_bytes: () => 33554432
  };
  const factory = config => new Promise(resolve => { config.instantiateWasm({}, () => resolve(module)); });
  const wasm = { instantiate: async path => {
    assert.equal(path, 'engine/hegame.wasm');
    if (options.wasmError) throw new Error('bad wasm');
    return options.directInstance ? { exports: {} } : { instance: { exports: {} } };
  } };
  const config = { scene: 'Assets/scene.he', texture: 'Assets/texture.png', audio: 'Assets/audio.wav', storageKey: 'save', resources: [{ path: 'Assets/scene.he', packagePath: 'Assets/scene.he.bin', bytes: 1 }] };
  return { calls, host: createHost(wx, factory, wasm, config, 'host-unit-test') };
}

(async () => {
  const originalError = console.error;
  console.error = () => {}; // Expected fault paths; assertions inspect structured evidence below.
  try {
    for (const directInstance of [false, true]) {
      const { host, calls } = fixture({ directInstance, saved: '{"schema":1}' });
      try {
        await host.start(); frame(100); frame(20000);
        assert.deepEqual(calls.delta, [0, 0.05]);
        assert.equal(calls.audio, 0, 'no audio before user gesture');
        assert.equal(calls.initSaved[0], '{"schema":1}');
        assert.deepEqual(calls.reads, ['Assets/scene.he.bin'], 'read packaged extension while retaining HE scene identity');
        callbacks.get('TouchStart')({ changedTouches: [{ identifier: 1, clientX: 150, clientY: 100 }] });
        assert.equal(calls.audio, 1);
        assert.deepEqual(calls.touches.at(-1), [0.25, 0.5, 1], 'logical coordinates, independent of DPR');
        callbacks.get('Hide')(); assert.equal(scheduled.size, 0);
        callbacks.get('Show')(); frame(999999); assert.equal(calls.delta.at(-1), 0);
        assert.equal(calls.touches.at(-1)[2], 0);
        await host.save(); assert(calls.saved.length > 0);
      } finally { host.shutdown(); }
      assert.equal(scheduled.size, 0); assert.equal(callbacks.size, 0); assert.equal(calls.shutdown, 1);
    }
    for (const option of ['noGL', 'readError', 'wasmError', 'storageReadError']) {
      const { host } = fixture({ [option]: true });
      await assert.rejects(host.start());
      assert(!host.isReady()); assert(host.getMetrics().errors.length > 0); assert.equal(scheduled.size, 0);
    }
    const damaged = fixture({ wrongSize: true });
    await assert.rejects(damaged.host.start(), /size mismatch: Assets\/scene.he \(package: Assets\/scene.he.bin, expected: 1, actual: 2\)/);
    assert.equal(damaged.calls.initSaved.length, 0, 'reject damaged package bytes before C++ initialization');
    assert(!damaged.host.isReady()); assert.equal(scheduled.size, 0);
    const { host } = fixture({ saveError: true });
    try { await host.start(); await assert.rejects(host.save()); assert(host.getMetrics().errors.some(value => value.includes('storage full'))); }
    finally { host.shutdown(); }
    const stalled = fixture({ readHang: true });
    const realSetTimeout = global.setTimeout;
    let testDeadline;
    try {
      global.setTimeout = fn => realSetTimeout(fn, 1);
      const deadline = new Promise((_, reject) => { testDeadline = realSetTimeout(() => reject(new Error('Resource wait escaped the startup timeout')), 1000); });
      await assert.rejects(Promise.race([stalled.host.start(), deadline]), /Wasm\/resource startup timed out/);
      assert(!stalled.host.isReady()); assert.equal(scheduled.size, 0);
    } finally { global.setTimeout = realSetTimeout; clearTimeout(testDeadline); stalled.host.shutdown(); }
    console.log('PASS host contracts: WX instance shapes, failures, touch/DPR, gesture audio, background timing, save feedback, shutdown');
  } finally { console.error = originalError; }
})().catch(error => { console.error(error); process.exitCode = 1; });
