'use strict';

// Native UI can be hidden by the platform launch page until a frame is
// presented. Keep local evidence as well; no network reporting or game saves.
function createBootDiagnostics(wx) {
  let stage = 'entry', done = false, failed = false, timer;
  let buildId = 'unknown', cancelPresentation;
  const history = [];
  function attempt(callback) { try { callback(); } catch (_) {} }
  function persist() {
    attempt(() => wx.setStorageSync('he-wechat-last-boot', { buildId, stage, history: history.slice(), done, failed }));
  }
  function fail(error) {
    if (failed) return;
    failed = true;
    clearTimeout(timer);
    if (cancelPresentation) cancelPresentation(error);
    const message = String(error && (error.stack || error.errMsg || error.message) || error);
    const record = { buildId, stage, message, history: history.slice() };
    console.error('[HE_BOOT_DIAGNOSTIC]', JSON.stringify(record));
    attempt(() => wx.hideLoading());
    attempt(() => wx.setStorageSync('he-wechat-last-boot-error', record));
    persist();
    attempt(() => wx.showModal({ title: 'HE 启动失败', content: buildId + '\n' + stage + '\n' + message.slice(0, 600), showCancel: false,
      fail: error => console.warn('[HE_DIAGNOSTIC_UI]', error && error.errMsg || String(error)) }));
  }
  function setStage(value) {
    stage = value;
    history.push({ stage, time: Date.now() });
    if (history.length > 16) history.shift();
    console.info('[HE_BOOT_STAGE]', stage);
    persist();
    if (!done && !failed) attempt(() => wx.showLoading({ title: 'HE ' + stage, mask: false }));
  }
  function identify(value, enableDebug = false) {
    buildId = value || 'unknown';
    console.info('[HE_BUILD]', buildId);
    persist();
    if (enableDebug && typeof wx.setEnableDebug === 'function')
      attempt(() => wx.setEnableDebug({ enableDebug: true,
        success: () => console.info('[HE_DEBUG]', 'enabled'),
        fail: error => console.warn('[HE_DEBUG]', error && error.errMsg || String(error)) }));
  }
  async function present(canvas, context) {
    setStage('bootstrap-frame');
    function draw() {
      context.viewport(0, 0, canvas.width, canvas.height);
      context.clearColor(.025, .04, .07, 1);
      context.clear(context.COLOR_BUFFER_BIT);
      const error = context.getError();
      if (error) throw new Error('Bootstrap canvas GL error: ' + error);
    }
    draw();
    await new Promise((resolve, reject) => {
      let frameId, deadline, settled = false;
      function finish(error) {
        if (settled) return;
        settled = true;
        cancelPresentation = undefined;
        clearTimeout(deadline);
        if (frameId !== undefined) cancelAnimationFrame(frameId);
        if (error) reject(error); else resolve();
      }
      cancelPresentation = error => finish(error);
      deadline = setTimeout(() => finish(new Error('Bootstrap frame scheduling timed out')), 2000);
      frameId = requestAnimationFrame(() => {
        try {
          draw();
          frameId = requestAnimationFrame(() => finish());
        } catch (error) { finish(error); }
      });
    });
    setStage('bootstrap-presented');
  }
  function ready() {
    if (done || failed) return;
    done = true;
    clearTimeout(timer);
    setStage('ready');
    attempt(() => wx.hideLoading());
  }
  timer = setTimeout(() => { if (!done) fail(new Error('Startup exceeded 30 seconds')); }, 30000);
  setStage('entry');
  return { stage: setStage, ready, fail, identify, present };
}
module.exports = { createBootDiagnostics };
