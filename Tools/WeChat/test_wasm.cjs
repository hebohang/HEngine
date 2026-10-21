'use strict';
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const assert = require('node:assert/strict');

(async () => {
  const build = path.resolve(process.argv[2] || 'out/agent/wechat-wasm-release');
  const exportsObject = {};
  // Explicitly omit DOM, Node process/require, fetch, XHR and browser globals.
  const sandbox = { module: { exports: exportsObject }, exports: exportsObject,
    WebAssembly, console, TextDecoder, TextEncoder, setTimeout, clearTimeout };
  vm.runInNewContext(fs.readFileSync(path.join(build, 'hegame.js'), 'utf8'), sandbox, { timeout: 5000 });
  const binary = fs.readFileSync(path.join(build, 'hegame.wasm'));
  let instance;
  const module = await sandbox.module.exports({ instantiateWasm(imports, receive) {
    WebAssembly.instantiate(binary, imports).then(result => { instance = result.instance; receive(instance); });
    return {};
  } });
  assert.equal(module._he_probe(), 1);
  assert.equal(module._he_heap_bytes(), 33554432);
  assert.equal(module._he_frame(0), 0, 'uninitialized C++ error must survive JS-based exception handling');
  assert.match(module.UTF8ToString(module._he_last_error()), /not initialized/);
  module.FS.mkdirTree('/project');
  const arguments_ = ['/project', 'Assets/missing.he', 'Assets/missing.png', ''].map(value => module.stringToNewUTF8(value));
  try {
    assert.equal(module._he_init(...arguments_), 0);
    assert.match(module.UTF8ToString(module._he_last_error()), /Failed to open JSON file/);
  } finally { arguments_.forEach(value => module._free(value)); }
  // Exercise the real scene, Catalog hash, generated reflection and C++ factory
  // even in the no-DOM harness. Only the deliberate absence of a GPU may fail.
  for (const asset of ['Touch.he', 'checker.png']) {
    module.FS.mkdirTree('/project/Assets/WeChatSample');
    module.FS.writeFile('/project/Assets/WeChatSample/' + asset,
      fs.readFileSync(path.resolve('Project/Assets/WeChatSample', asset)));
  }
  const sceneArguments = ['/project', 'Assets/WeChatSample/Touch.he', 'Assets/WeChatSample/checker.png', '']
    .map(value => module.stringToNewUTF8(value));
  try {
    assert.equal(module._he_init(...sceneArguments), 0);
    assert.match(module.UTF8ToString(module._he_last_error()), /Cannot bind the host WebGL 2 context/);
  } finally { sceneArguments.forEach(value => module._free(value)); }
  const memory = Object.values(instance.exports).find(value => value instanceof WebAssembly.Memory);
  assert(memory && !(memory.buffer instanceof SharedArrayBuffer), 'single-thread non-shared linear memory');
  assert.equal(module._he_shutdown(), 1);
  console.log(JSON.stringify({ status: 'PASS', evidence: 'node-no-DOM-wasm', cppAbi: 1, wasmBytes: binary.length,
    linearMemoryBytes: memory.buffer.byteLength, sharedMemory: false, cppExceptions: 'JS emulation exercised',
    omittedHostAPIs: ['DOM', 'window', 'document', 'fetch', 'XMLHttpRequest', 'process', 'require'] }));
})().catch(error => { console.error(error); process.exitCode = 1; });
