import {dotnet} from './_framework/dotnet.js';
import {createScriptTransport} from './engine-transport.js';

const started = performance.now(), canvas = document.getElementById('game');
const status = document.getElementById('status'), logElement = document.getElementById('logs');
const entries = [], metrics = {frames: 0, frameCpuMilliseconds: 0, maxFrameCpuMilliseconds: 0, firstGameFrameMs: null, gc: []};
let engine, transport, managed, manifest, running = false, enabled = true, lastTime = null;
let moveX = 0, moveY = 0, pointerX = 640, pointerY = 360, pointerDown = false;
const keys = new Set(), namespace = 'HEngine.ScriptSample.v1.';
function log(...args) {
    const text = args.join(' '); entries.push(text); if (entries.length > 80) entries.shift();
    logElement.textContent = entries.join('\n'); logElement.scrollTop = logElement.scrollHeight;
    console.log('[HEngine]', text);
}
function check(value, operation) {
    if (!value) throw new Error(operation + ': ' + engine.UTF8ToString(engine._he_last_error()));
}
function strings(values, callback) {
    const pointers = values.map(value => engine.stringToNewUTF8(value));
    try { return callback(...pointers); } finally { pointers.forEach(pointer => engine._free(pointer)); }
}
function fields() {
    const pointer = engine._he_script_fields();
    return pointer ? JSON.parse(engine.UTF8ToString(pointer)).fields : null;
}
function initialize(saved = '') {
    strings(['/project', manifest.scene, manifest.texture, saved], (...args) => check(engine._he_init(...args), 'Initialize scene'));
    check(engine._he_resize(canvas.width, canvas.height), 'Resize'); enabled = Boolean(engine._he_script_is_enabled());
    document.getElementById('enable').textContent = enabled ? '禁用脚本' : '启用脚本';
    lastTime = null;
}
function input() {
    if (!engine || !running) return;
    moveX = Number(keys.has('d') || keys.has('arrowright')) - Number(keys.has('a') || keys.has('arrowleft'));
    moveY = Number(keys.has('w') || keys.has('arrowup')) - Number(keys.has('s') || keys.has('arrowdown'));
    engine._he_script_input(moveX, moveY, pointerX, pointerY, pointerDown ? 1 : 0);
}
function resize() {
    const ratio = Math.min(devicePixelRatio || 1, 2), rect = canvas.getBoundingClientRect();
    canvas.width = Math.max(1, Math.round(rect.width * ratio)); canvas.height = Math.max(1, Math.round(rect.height * ratio));
    if (engine && running) check(engine._he_resize(canvas.width, canvas.height), 'Resize');
}
function frame(now) {
    if (!running) return;
    try {
        const before = performance.now();
        check(engine._he_frame(lastTime === null ? 0 : Math.min((now - lastTime) / 1000, 0.05)), 'Game frame');
        lastTime = now; metrics.frames++;
        const elapsed = performance.now() - before;
        metrics.frameCpuMilliseconds += elapsed; metrics.maxFrameCpuMilliseconds = Math.max(metrics.maxFrameCpuMilliseconds, elapsed);
        if (metrics.firstGameFrameMs === null) { canvas.getContext('webgl2').finish(); metrics.firstGameFrameMs = performance.now() - started; log('First rendered game frame ms=' + metrics.firstGameFrameMs.toFixed(2)); }
        const current = fields();
        status.textContent = current ? `C# 游戏运行中 · 分数 ${current.score?.value ?? '?'} · ${enabled ? '已启用' : '已禁用'}` : '实体已销毁，游戏仍在运行';
        requestAnimationFrame(frame);
    } catch (error) { running = false; status.textContent = '游戏宿主发生错误'; log(error.stack || error); }
}
async function verified(path) {
    const entry = manifest.assets.find(asset => asset.path === path);
    if (!entry) throw new Error('Asset absent from the package manifest: ' + path);
    const response = await fetch(path, {cache: 'no-cache'}); if (!response.ok) throw new Error('HTTP ' + response.status + ': ' + path);
    const bytes = await response.arrayBuffer();
    const hash = Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256', bytes)), value => value.toString(16).padStart(2, '0')).join('');
    if (hash !== entry.sha256) throw new Error('Package integrity mismatch: ' + path);
    return new Uint8Array(bytes);
}
try {
    manifest = await (await fetch('game-manifest.json', {cache:'no-cache'})).json();
    const runtime = await dotnet.withDiagnosticTracing(false).create();
    managed = (await runtime.getAssemblyExports(runtime.getConfig().mainAssemblyName)).HEngine.Scripting.WasmEntry;
    const metadata = JSON.parse(managed.Text(manifest.api.managedOperations.Describe, 0, '', ''));
    if (managed.LastError() || metadata.identity !== manifest.managedIdentity ||
        metadata.gameplaySourceIdentity !== manifest.gameplaySourceIdentity ||
        JSON.stringify(metadata) !== JSON.stringify(manifest.registration))
        throw new Error('Managed registry differs from this exported build.');
    resize();
    const context = canvas.getContext('webgl2', {alpha:false, antialias:false, depth:true, stencil:true, preserveDrawingBuffer:true});
    if (!context) throw new Error('WebGL 2 is required.');
    engine = await globalThis.createHEngine({canvas, preinitializedWebGLContext:context, wasmBinary:await verified('hegame.wasm'),
        print:log, printErr:log, onAbort:reason => { running = false; log('Engine abort: ' + reason); }});
    engine.heManaged = managed;
    engine.hePlatform = {readSave:key => localStorage.getItem(namespace + key) || '', writeSave:(key, value) => localStorage.setItem(namespace + key, value)};
    transport = createScriptTransport(engine);
    runtime.setModuleImports('HEngine.ScriptTransport', {call: transport.call});
    for (const asset of manifest.assets.filter(asset => asset.path.startsWith('project/'))) {
        const target = '/' + asset.path; engine.FS.mkdirTree(target.slice(0, target.lastIndexOf('/'))); engine.FS.writeFile(target, await verified(asset.path));
    }
    initialize(localStorage.getItem(namespace + 'scene') || ''); running = true;
    document.getElementById('identity').textContent = 'Build ' + manifest.buildId + ' · C# assembly ' + manifest.managedIdentity;
    for (const button of document.querySelectorAll('button')) button.disabled = false;
    document.getElementById('save').onclick = () => {
        try { const pointer = engine._he_save(); check(pointer, 'Save scene'); localStorage.setItem(namespace + 'scene', engine.UTF8ToString(pointer)); location.reload(); }
        catch (error) { log(error.stack || error); }
    };
    document.getElementById('restart').onclick = () => { try { initialize(); log('Re-entered source scene.'); } catch (error) { log(error.stack || error); } };
    document.getElementById('enable').onclick = () => { try { check(engine._he_script_enabled(!enabled), 'Enable'); enabled = !enabled; document.getElementById('enable').textContent = enabled ? '禁用脚本' : '启用脚本'; } catch (error) { log(error.stack || error); } };
    document.getElementById('destroy').onclick = () => { try { check(engine._he_script_destroy(), 'Destroy entity'); } catch (error) { log(error.stack || error); } };
    document.getElementById('fault').onclick = () => strings(['throwOnUpdate', JSON.stringify({type:'bool',value:true})], (id,value) => check(engine._he_script_set_field(id,value), 'Set exception field'));
    document.getElementById('collect').onclick = () => {
        const before = performance.now(); const bytes = managed.Execute(manifest.api.managedOperations.Collect, 0, 0, 0, 0, '');
        if (managed.LastError()) throw new Error(managed.LastError());
        const result = {milliseconds:performance.now()-before, managedHeapBytes:bytes}; metrics.gc.push(result); log('GC ' + JSON.stringify(result));
    };
    for (const name of ['keydown','keyup']) addEventListener(name, event => {
        const key = event.key.toLowerCase(); if (!['w','a','s','d','arrowleft','arrowright','arrowup','arrowdown'].includes(key)) return;
        event.preventDefault(); if (name === 'keydown') keys.add(key); else keys.delete(key); input();
    });
    for (const name of ['pointerdown','pointermove','pointerup','pointercancel']) canvas.addEventListener(name, event => {
        const rect = canvas.getBoundingClientRect(); pointerX = (event.clientX-rect.left)*canvas.width/rect.width; pointerY = (event.clientY-rect.top)*canvas.height/rect.height;
        if (name === 'pointerdown') { pointerDown = true; canvas.setPointerCapture(event.pointerId); }
        if (name === 'pointerup' || name === 'pointercancel') pointerDown = false;
        input();
    });
    addEventListener('blur', () => { keys.clear(); pointerDown = false; input(); });
    addEventListener('resize', resize);
    document.addEventListener('visibilitychange', () => { keys.clear(); pointerDown = false; input(); check(engine._he_visible(!document.hidden), 'Visibility'); lastTime=null; });
    addEventListener('pagehide', () => { running=false; engine._he_shutdown(); transport.dispose(); }, {once:true});
    globalThis.HEGame = {manifest, metrics, entries, fields, engine, managed, transport, get running(){return running;},
        memory:() => ({engineLinearBytes:engine._he_heap_bytes(), dotnetLinearBytes:runtime.Module?.HEAPU8?.byteLength ?? null})};
    requestAnimationFrame(frame);
} catch (error) { status.textContent = '游戏启动失败'; log(error.stack || error); globalThis.HEGame = {error:String(error.stack || error), entries}; }
