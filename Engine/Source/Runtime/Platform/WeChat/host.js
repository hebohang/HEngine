'use strict';

// Host API bridge only. Scene simulation and gameplay stay in C++.
function createHost(wx, createEngine, wasm, config, evidenceMode = 'wechat-host', diagnostics = null, scripts = null) {
  let module, canvas, context, audio, frameId, lastTime = null, touchId = null;
  let visible = true, ready = false, stopped = false, audioPlaying = false, resumeAudio = false;
  let logicalWidth = 1, logicalHeight = 1, ratio = 1;
  let saveTail = Promise.resolve();
  const registrations = [];
  const started = Date.now();
  const metrics = { mode: evidenceMode, firstInteractiveMs: null, frames: 0, cpuFrameMs: [], errors: [] };
  metrics.timingScope = 'Date.now millisecond resolution; JS + C++ simulation/render submission; excludes GPU execution';
  metrics.audioPlayEvents = 0; metrics.audioEndedEvents = 0;
  const fs = wx.getFileSystemManager();
  const report = (stage, error) => {
    const message = `${stage}: ${error && (error.stack || error.errMsg || error.message) || error}`;
    metrics.errors.push(message);
    if (metrics.errors.length > 32) metrics.errors.shift();
    console.error('[HE]', message);
  };
  function listen(on, off, callback) {
    if (typeof wx[on] !== 'function') throw new Error(`Missing host API ${on}`);
    const guarded = (...args) => { try { callback(...args); } catch (error) { report(on, error); shutdown(); } };
    wx[on](guarded); registrations.push(() => { if (typeof wx[off] === 'function') wx[off](guarded); });
  }
  function read(path) {
    if (!path || path.startsWith('/') || path.includes('..') || path.includes('\\') || path.includes(':'))
      return Promise.reject(new Error(`Invalid packaged resource path: ${path}`));
    return new Promise((resolve, reject) => fs.readFile({ filePath: path,
      success: result => resolve(result.data), fail: reject }));
  }
  function check(value, operation) {
    if (!value) throw new Error(`${operation}: ${module.UTF8ToString(module._he_last_error())}`);
  }
  function callInit(saved) {
    const values = ['/project', config.scene, config.texture, saved || ''].map(s => module.stringToNewUTF8(s));
    try { check(module._he_init(...values), 'C++ initialization'); }
    finally { values.forEach(value => module._free(value)); }
  }
  function clearTouch() {
    touchId = null;
    if (module && ready) {
      if (scripts) module._he_script_input(0, 0, 0, 0, 0);
      else module._he_touch(0, 0, 0);
    }
  }
  function resize() {
    const info = typeof wx.getWindowInfo === 'function' ? wx.getWindowInfo() : wx.getSystemInfoSync();
    logicalWidth = info.windowWidth; logicalHeight = info.windowHeight; ratio = info.pixelRatio || 1;
    if (![logicalWidth, logicalHeight, ratio].every(v => Number.isFinite(v) && v > 0)) throw new Error('Invalid canvas sizing response');
    canvas.width = Math.max(1, Math.round(logicalWidth * ratio));
    canvas.height = Math.max(1, Math.round(logicalHeight * ratio));
    if (module && ready) check(module._he_resize(canvas.width, canvas.height), 'resize');
    clearTouch();
  }
  function save() {
    if (!ready || stopped) return saveTail;
    const pointer = module._he_save(); check(pointer, 'save serialization');
    const value = module.UTF8ToString(pointer);
    // Namespace is persistent user storage; assets never enter this namespace.
    saveTail = saveTail.catch(() => {}).then(() => new Promise((resolve, reject) =>
      wx.setStorage({ key: config.storageKey, data: value, success: resolve, fail: reject })));
    saveTail.catch(error => report('persistent save failed', error));
    return saveTail;
  }
  function touch(event, begin) {
    if (!ready || !visible || stopped) return;
    const touches = event.changedTouches || event.touches || [];
    if (begin && touchId === null && touches.length) {
      touchId = touches[0].identifier;
      try { audio.seek(0); audio.play(); } catch (error) { report('gesture audio', error); }
    }
    const current = touches.find(item => item.identifier === touchId);
    if (!current) return;
    const x = current.clientX === undefined ? current.x : current.clientX;
    const y = current.clientY === undefined ? current.y : current.clientY;
    if (!Number.isFinite(x) || !Number.isFinite(y)) { clearTouch(); return; }
    if (scripts) module._he_script_input(0, 0, x * ratio, y * ratio, 1);
    else module._he_touch((x / logicalWidth * 2 - 1) * logicalWidth / logicalHeight, 1 - y / logicalHeight * 2, 1);
  }
  function release(event) {
    if ((event.changedTouches || []).some(touch => touch.identifier === touchId)) {
      clearTouch();
      try {
        save();
        if (scripts && scripts.onTouchReleased) scripts.onTouchReleased();
      } catch (error) { report('save after touch', error); }
    }
  }
  function tick(timestamp) {
    frameId = null;
    if (stopped || !ready || !visible) return;
    const now = Number.isFinite(timestamp) ? timestamp : Date.now();
    const delta = lastTime === null ? 0 : Math.max(0, Math.min((now - lastTime) / 1000, 0.05));
    lastTime = now;
    const before = Date.now();
    try {
      check(module._he_frame(delta), 'frame');
      metrics.frames++;
      if (scripts && metrics.frames === 1) {
        context.finish(); metrics.firstRenderedFrameMs = Date.now() - started;
      }
      if (metrics.frames === 1 && diagnostics) diagnostics.ready();
      metrics.wasmLinearMemoryBytes = module._he_heap_bytes();
      if (metrics.firstInteractiveMs === null) metrics.firstInteractiveMs = Date.now() - started;
      if (metrics.frames > 30 && metrics.cpuFrameMs.length < 300) metrics.cpuFrameMs.push(Date.now() - before);
      if (metrics.frames === 120) console.info('[HE_METRICS]', JSON.stringify(metrics));
      frameId = requestAnimationFrame(tick);
    } catch (error) { report('render/simulation stopped', error); if (diagnostics) diagnostics.fail(error); shutdown(); }
  }
  function schedule() { if (ready && visible && !stopped && frameId === null) frameId = requestAnimationFrame(tick); }
  async function start() {
    frameId = null;
    try {
      if (!wasm || typeof wasm.instantiate !== 'function') throw new Error('WXWebAssembly.instantiate unavailable');
      if (diagnostics) diagnostics.stage('webgl2');
      canvas = wx.createCanvas();
      resize();
      context = canvas.getContext('webgl2', { alpha: false, depth: true, stencil: true, antialias: false, premultipliedAlpha: false });
      if (!context || typeof context.createVertexArray !== 'function' || typeof context.texStorage2D !== 'function')
        throw new Error('This WeChat environment did not supply WebGL 2; no WebGL 1 fallback exists');
      metrics.glVersion = context.getParameter(context.VERSION);
      metrics.renderer = context.getParameter(context.RENDERER);
      metrics.canvas = [canvas.width, canvas.height];
      // Present on the same screen canvas before synchronous C++ initialization.
      // Otherwise a failure before the game's first draw may leave native error
      // UI behind the platform launch screen. This is not a gameplay frame.
      if (diagnostics && diagnostics.present) await diagnostics.present(canvas, context);
      if (stopped) throw new Error('Startup cancelled');
      audio = wx.createInnerAudioContext();
      audio.src = config.audio; audio.autoplay = false; audio.loop = false;
      audio.onPlay(() => { audioPlaying = true; metrics.audioPlayEvents++; });
      audio.onEnded(() => { audioPlaying = false; metrics.audioEndedEvents++; });
      audio.onError(error => { if (!stopped) report('audio', error); });
      listen('onTouchStart', 'offTouchStart', event => touch(event, true));
      listen('onTouchMove', 'offTouchMove', event => touch(event, false));
      listen('onTouchEnd', 'offTouchEnd', release);
      listen('onTouchCancel', 'offTouchCancel', () => { clearTouch(); });
      listen('onWindowResize', 'offWindowResize', () => { try { resize(); } catch (error) { report('resize', error); } });
      listen('onHide', 'offHide', () => {
        visible = false; lastTime = null; clearTouch();
        if (ready) { check(module._he_visible(0), 'hide'); save(); }
        if (frameId !== null) { cancelAnimationFrame(frameId); frameId = null; }
        resumeAudio = audioPlaying; audio.pause(); audioPlaying = false;
      });
      listen('onShow', 'offShow', () => {
        visible = true; lastTime = null; clearTouch();
        if (ready) check(module._he_visible(1), 'show');
        if (resumeAudio) { resumeAudio = false; audio.play(); }
        schedule();
      });
      if (typeof wx.onAudioInterruptionBegin === 'function') {
        listen('onAudioInterruptionBegin', 'offAudioInterruptionBegin', () => { resumeAudio = audioPlaying; audio.pause(); audioPlaying = false; });
        listen('onAudioInterruptionEnd', 'offAudioInterruptionEnd', () => { if (visible && resumeAudio) { resumeAudio = false; audio.play(); } });
      }
      const filesPromise = Promise.all(config.resources.map(async item => [item.path, await read(item.packagePath || item.path)]));
      if (diagnostics) diagnostics.stage('wasm');
      // getStorageInfo distinguishes a missing initial save from IO/permission failure.
      const savedPromise = new Promise((resolve, reject) => wx.getStorageInfo({
        success: info => {
          if (!info.keys.includes(config.storageKey)) return resolve('');
          wx.getStorage({ key: config.storageKey, success: result => {
            if (typeof result.data !== 'string') return reject(new Error('Stored state is not a string'));
            resolve(result.data);
          }, fail: reject });
        }, fail: reject
      }));
      let rejectBoot;
      const bootFailure = new Promise((_, reject) => { rejectBoot = reject; });
      const prerequisites = Promise.all([filesPromise, savedPromise]);
      prerequisites.catch(rejectBoot);
      let timeout = setTimeout(() => rejectBoot(new Error('Wasm/resource startup timed out after 30 seconds')), 30000);
      try {
        module = await Promise.race([createEngine({
          canvas, preinitializedWebGLContext: context,
          print: (...args) => console.info('[HE C++]', ...args), printErr: (...args) => console.error('[HE C++]', ...args),
          onAbort: reason => rejectBoot(new Error(`Wasm abort: ${reason}`)),
          instantiateWasm(imports, receive) {
            Promise.resolve().then(() => wasm.instantiate('engine/hegame.wasm', imports)).then(result => {
              if (diagnostics) diagnostics.stage('wasm-ready');
              const instance = result.instance || result;
              if (!instance || !instance.exports) throw new Error('WXWebAssembly returned no instance.exports');
              if (!stopped) receive(instance, result.module);
            }).catch(rejectBoot);
            return {};
          }
        }), bootFailure]);
        if (diagnostics) diagnostics.stage('resources');
        await Promise.race([prerequisites, bootFailure]);
      } finally { clearTimeout(timeout); }
      const [files, saved] = await prerequisites;
      if (stopped) throw new Error('Startup cancelled');
      module.FS.mkdirTree('/project');
      for (const [path, data] of files) {
        const item = config.resources.find(resource => resource.path === path);
        if (!(data instanceof ArrayBuffer)) throw new Error(`readFile must return ArrayBuffer: ${path}`);
        const bytes = new Uint8Array(data);
        if (bytes.byteLength !== item.bytes)
          throw new Error(`Packaged resource size mismatch: ${path} (package: ${item.packagePath || path}, expected: ${item.bytes}, actual: ${bytes.byteLength})`);
        const target = '/project/' + path;
        module.FS.mkdirTree(target.slice(0, target.lastIndexOf('/')));
        module.FS.writeFile(target, bytes);
      }
      if (diagnostics) diagnostics.stage('scene');
      if (scripts) await scripts.attach(module);
      if (stopped) throw new Error('Startup cancelled');
      callInit(saved);
      ready = true;
      check(module._he_resize(canvas.width, canvas.height), 'initial resize');
      check(module._he_visible(visible ? 1 : 0), 'initial visibility');
      if (diagnostics) diagnostics.stage('first-frame');
      schedule();
      return api;
    } catch (error) { report('startup failed', error); shutdown(); throw error; }
  }
  function shutdown() {
    stopped = true; clearTouch();
    if (frameId !== null && frameId !== undefined) cancelAnimationFrame(frameId);
    frameId = null;
    registrations.splice(0).reverse().forEach(off => off());
    if (audio) { audio.stop(); audio.destroy(); audio = null; }
    if (module) {
      module._he_shutdown();
      if (scripts) scripts.dispose();
      module = null;
    }
    ready = false;
  }
  const api = { start, save, shutdown, getMetrics: () => JSON.parse(JSON.stringify(metrics)), isReady: () => ready,
    scripts: scripts ? scripts.controls : undefined,
    reload: async (restore = true) => {
      if (!ready || stopped) throw new Error('Scene is not running');
      if (restore) await save();
      clearTouch(); callInit(restore ? wx.getStorageSync(config.storageKey) : '');
      check(module._he_resize(canvas.width, canvas.height), 'reloaded resize'); lastTime = null;
    }
  };
  return api;
}
module.exports = { createHost };
