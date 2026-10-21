'use strict';
// Auxiliary browser diagnostics. All simulation is the same C++ Wasm artifact.
const callbacks = new Map();
const fire = (name, value = {}) => (callbacks.get(name) || []).forEach(fn => fn(value));
const canvas = document.getElementById('game');
const wx = {
  setStorageSync: (key, value) => localStorage.setItem(key, JSON.stringify(value)),
  showLoading: ({ title }) => { document.getElementById('state').textContent = title; }, hideLoading() {},
  showModal: ({ content }) => { document.getElementById('state').textContent = content; },
  createCanvas: () => canvas,
  getWindowInfo: () => ({ windowWidth: innerWidth, windowHeight: innerHeight, pixelRatio: devicePixelRatio }),
  getFileSystemManager: () => ({ readFile: ({ filePath, success, fail }) => fetch(filePath).then(response => {
    if (!response.ok) throw new Error(`HTTP ${response.status}: ${filePath}`); return response.arrayBuffer();
  }).then(data => success({ data })).catch(fail) }),
  getStorageInfo: ({ success }) => success({ keys: Object.keys(localStorage) }),
  getStorage: ({ key, success, fail }) => { try { success({ data: localStorage.getItem(key) }); } catch (error) { fail(error); } },
  setStorage: ({ key, data, success, fail }) => { try { localStorage.setItem(key, data); success({}); } catch (error) { fail(error); } },
  createInnerAudioContext: () => {
    const player = new Audio();
    return {
      set src(value) { player.src = value; }, set autoplay(value) { player.autoplay = value; }, set loop(value) { player.loop = value; },
      seek: time => { player.currentTime = time; }, play: () => { player.play().catch(error => fire('AudioError', error)); },
      pause: () => player.pause(), stop: () => { player.pause(); player.currentTime = 0; }, destroy: () => { player.pause(); player.src = ''; },
      onPlay: fn => player.addEventListener('play', fn), onEnded: fn => player.addEventListener('ended', fn),
      onError: fn => { player.addEventListener('error', fn); wx.onAudioError(fn); }
    };
  }
};
for (const name of ['TouchStart', 'TouchMove', 'TouchEnd', 'TouchCancel', 'Hide', 'Show', 'WindowResize', 'AudioInterruptionBegin', 'AudioInterruptionEnd', 'AudioError']) {
  wx['on' + name] = fn => callbacks.set(name, [...(callbacks.get(name) || []), fn]);
  wx['off' + name] = fn => callbacks.set(name, (callbacks.get(name) || []).filter(item => item !== fn));
}
for (const [event, name] of [['pointerdown', 'TouchStart'], ['pointermove', 'TouchMove'], ['pointerup', 'TouchEnd'], ['pointercancel', 'TouchCancel']]) {
  canvas.addEventListener(event, value => {
    if (event === 'pointerdown') canvas.setPointerCapture(value.pointerId);
    fire(name, { changedTouches: [{ identifier: value.pointerId, clientX: value.clientX, clientY: value.clientY }] });
  });
}
addEventListener('resize', () => fire('WindowResize'));
document.addEventListener('visibilitychange', () => fire(document.hidden ? 'Hide' : 'Show'));
const wasm = { instantiate: async (path, imports) => WebAssembly.instantiate(await (await fetch(path)).arrayBuffer(), imports) };
const diagnostics = createBootDiagnostics(wx);
diagnostics.identify(config.buildId);
const session = createHost(wx, createEngine, wasm, config, 'auxiliary-browser', diagnostics);
window.HEBrowser = { session, fire, config };
session.start().then(() => { document.getElementById('state').textContent = 'HE C++ ready · click/drag to move, change color, play sound and save'; })
  .catch(error => { diagnostics.fail(error); document.getElementById('state').textContent = error.stack; });
