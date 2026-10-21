'use strict';
const diagnostics = require('./boot-diagnostics.js').createBootDiagnostics(wx);
try {
const config = require('./he-config.js');
diagnostics.identify(config.buildId, config.enableDebug === true);
const { createHost } = require('./host.js');
// Emscripten checks the standard namespace; instantiate itself is explicitly
// routed to WXWebAssembly's package-path API by host.js.
const globals = typeof GameGlobal !== 'undefined' ? GameGlobal : globalThis;
if (typeof WXWebAssembly === 'undefined') throw new Error('WXWebAssembly is required');
if (!globals.WebAssembly) globals.WebAssembly = WXWebAssembly;
diagnostics.stage('load-js');
const createEngine = require('./engine/hegame.js');
const session = createHost(wx, createEngine, WXWebAssembly, config, 'wechat-host', diagnostics);
globals.HEWeChatSession = session;
session.start().catch(error => diagnostics.fail(error));
} catch (error) { diagnostics.fail(error); }
