'use strict';
const diagnostics = require('./boot-diagnostics.js').createBootDiagnostics(wx);
const config = require('./he-config.js');
diagnostics.identify(config.buildId, true);
const globals = typeof GameGlobal !== 'undefined' ? GameGlobal : globalThis;
const metrics = {gc: []};
let engine, transport, runtime, managed;
function check(value) {
    if (!value) throw new Error(engine.UTF8ToString(engine._he_last_error()));
}
function field(id, value) {
    const name = engine.stringToNewUTF8(id), data = engine.stringToNewUTF8(JSON.stringify(value));
    try { check(engine._he_script_set_field(name, data)); }
    finally { engine._free(name); engine._free(data); }
}
const scripts = {
    onTouchReleased() {
        const snapshot = scripts.controls.snapshot();
        const fields = JSON.parse(snapshot.script.Fields).fields;
        wx.showToast({title:'C# 分数 ' + fields.score.value,icon:'none',duration:1200});
    },
    controls: {
        snapshot: () => JSON.parse(engine.UTF8ToString(engine._he_save())),
        enabled: value => check(engine._he_script_enabled(value ? 1 : 0)),
        destroy: () => check(engine._he_script_destroy()),
        fault: () => field('throwOnUpdate', {type:'bool', value:true}),
        collect: () => {
            const start = Date.now(), bytes = managed.Execute(config.api.managedOperations.Collect, 0, 0, 0, 0, '');
            if (managed.LastError()) throw new Error(managed.LastError());
            const result = {milliseconds:Date.now()-start, managedHeapBytes:bytes}; metrics.gc.push(result); return result;
        },
        memory: () => ({engineLinearBytes:engine._he_heap_bytes(), dotnetLinearBytes:runtime.Module?.HEAPU8?.byteLength ?? null}),
        metrics: () => ({...metrics, bridge:transport.metrics}),
        identity: () => ({buildId:config.buildId, gameplaySourceIdentity:config.gameplaySourceIdentity, managedIdentity:config.managedIdentity})
    },
    async attach(module) {
        engine = module;
        diagnostics.stage('mono-subpackage');
        await new Promise((resolve,reject) => wx.loadSubpackage({name:'managed',success:resolve,fail:reject}));
        diagnostics.stage('mono');
        runtime = await require('./wechat-runtime.js').createManagedRuntime(require('./runtime-config.js'));
        managed = (await runtime.getAssemblyExports(runtime.getConfig().mainAssemblyName)).HEngine.Scripting.WasmEntry;
        const metadata = JSON.parse(managed.Text(config.api.managedOperations.Describe, 0, '', ''));
        if (managed.LastError() || metadata.identity !== config.managedIdentity ||
            metadata.gameplaySourceIdentity !== config.gameplaySourceIdentity ||
            JSON.stringify(metadata) !== JSON.stringify(config.registration))
            throw new Error('Managed registry differs from the exported build');
        engine.heManaged = managed;
        engine.hePlatform = {
            readSave: key => {
                const value = wx.getStorageSync(config.storageKey + '.' + key);
                if (value === '') return '';
                if (typeof value !== 'string') throw new Error('Script save must contain a string');
                return value;
            },
            writeSave: (key, value) => wx.setStorageSync(config.storageKey + '.' + key, value)
        };
        transport = require('./engine-transport.js').createScriptTransport(engine);
        runtime.setModuleImports('HEngine.ScriptTransport', {call:transport.call});
        diagnostics.stage('managed-scene');
        console.info('[HE_MANAGED_IDENTITY]', JSON.stringify(scripts.controls.identity()));
    },
    dispose() { if (transport) { transport.dispose(); transport = null; } }
};
try {
    if (typeof WXWebAssembly === 'undefined') throw new Error('WXWebAssembly is required');
    if (!globals.WebAssembly) globals.WebAssembly = WXWebAssembly;
    diagnostics.stage('load-js');
    const session = require('./host.js').createHost(wx, require('./engine/hegame.js'), WXWebAssembly,
        config, 'wechat-csharp-game', diagnostics, scripts);
    globals.HEWeChatSession = session;
    session.start().catch(error => diagnostics.fail(error));
} catch (error) { diagnostics.fail(error); }
