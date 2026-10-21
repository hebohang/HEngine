'use strict';
const canvas = wx.createCanvas();
const context = canvas.getContext('2d');
const started = Date.now();
const originalFetch = globalThis.fetch;
const originalWebAssembly = globalThis.WebAssembly;
const status = { phase: 'boot', calls: 0 };
const draw = () => {
    context.fillStyle = '#18202b'; context.fillRect(0, 0, canvas.width, canvas.height);
    context.fillStyle = '#ffffff'; context.font = '14px sans-serif';
    const lines = ['C# runtime probe (not engine game)', ...JSON.stringify(status, null, 2).split('\n')];
    lines.forEach((line, i) => context.fillText(line.slice(0, 70), 8, 25 + 19 * i));
};
const fail = error => {
    status.phase = 'failed'; status.error = String(error.stack || error);
    console.error('[he-csharp-probe-failed]', status.error); draw();
};
globalThis.heCSharpProbe = status;
draw();
const config = require('./runtime-config.js');
// Keep .NET host shims local to its transformed modules. The C++ engine host
// must retain its own fetch/WebAssembly bindings and independent memory.
const runtimeEnvironment = require('./runtime-env.js');
Object.setPrototypeOf(runtimeEnvironment, new Proxy(globalThis, {
    get: (target, key) => Reflect.get(target, key, target)
}));
const fs = wx.getFileSystemManager();
const packageBuffers = new WeakMap();
const packageName = url => {
    const name = String(url).split('/').pop().split('?')[0];
    if (!/^[a-zA-Z0-9_.-]+$/.test(name)) throw new Error('Invalid runtime asset path');
    return 'runtime/' + name;
};
runtimeEnvironment.fetch = async url => {
    const data = await new Promise((resolve, reject) => fs.readFile({
        filePath: packageName(url) + '.bin', success: result => resolve(result.data), fail: reject
    }));
    packageBuffers.set(data, packageName(url));
    return { ok: true, status: 200, url: String(url), headers: { get: () => null },
        arrayBuffer: async () => data, json: async () => { throw new Error('JSON asset request unsupported'); } };
};
(async () => {
    status.features = {
        wasm: typeof WXWebAssembly, bigInt: typeof BigInt, textDecoder: typeof TextDecoder,
        url: typeof URL, webAssembly: typeof WebAssembly
    };
    console.log('[he-csharp-probe-features]', JSON.stringify(status.features));
    console.log('[he-csharp-probe-wxapi]', Object.getOwnPropertyNames(WXWebAssembly).join(','));
    try { console.log('[he-csharp-probe-function]', new Function('return 42')()); }
    catch (error) { console.log('[he-csharp-probe-function]', String(error)); }
    console.log('[he-csharp-probe-function-source]', (function heRuntimeMarker() { return { runtime_idx: 1 }; }).toString());
    const resolveWasm = buffer => {
        const asset = packageBuffers.get(buffer);
        if (asset !== 'runtime/' + config.resources.wasmNative[0].name)
            throw new Error('Dynamic or unknown Wasm compilation is not supported by this host');
        return asset;
    };
    // Adapt the documented package-path API; never pretend to compile dynamic bytes.
    const features = require('./runtime-features.js');
    const supportedFeatures = new Map();
    for (const [bytes, file] of Object.entries(features)) {
        try { await WXWebAssembly.compile(file); supportedFeatures.set(bytes, true); }
        catch (error) { supportedFeatures.set(bytes, false); console.warn('[he-csharp-probe-feature]', file, String(error)); }
    }
    const compiledPaths = new WeakMap();
    runtimeEnvironment.WebAssembly = Object.assign({}, WXWebAssembly, {
        validate: bytes => {
            const key = Array.from(bytes).join(',');
            if (!supportedFeatures.has(key)) throw new Error('Unknown Wasm feature probe');
            return supportedFeatures.get(key);
        },
        compile: async buffer => {
            const file = resolveWasm(buffer);
            const module = await WXWebAssembly.compile(file);
            compiledPaths.set(module, file);
            return module;
        },
        Module: function(buffer) {
            const file = resolveWasm(buffer);
            const module = new WXWebAssembly.Module(file);
            compiledPaths.set(module, file);
            return module;
        },
        instantiate: async (module, imports) => {
            const file = compiledPaths.get(module);
            if (!file) throw new Error('Unknown Wasm module');
            const result = await WXWebAssembly.instantiate(file, imports);
            status.wasmInstantiated = true;
            return result.instance || result;
        }
    });
    const builder = require('./runtime/dotnet.js').dotnet;
    const runtime = await builder.withModuleConfig({
        print: text => console.log('[dotnet]', text), printErr: text => console.error('[dotnet]', text) })
        .withDiagnosticTracing(true).create();
    console.log('[he-csharp-probe-main-exit]', await runtime.runMain());
    const exports = await runtime.getAssemblyExports(runtime.getConfig().mainAssemblyName);
    console.log('[he-csharp-probe-exports]', Object.keys(exports).join(','), runtime.getConfig().mainAssemblyName);
    const probe = exports.Probe;
    status.identity = probe.Identity();
    status.first = probe.Step(7); status.second = probe.Step(11);
    status.runtime = probe.ExerciseRuntime();
    status.hostGlobalsUnchanged = globalThis.fetch === originalFetch && globalThis.WebAssembly === originalWebAssembly;
    if (!status.hostGlobalsUnchanged) throw new Error('The .NET probe changed engine host globals');
    if (status.first !== 7 || status.second !== 18 || !status.runtime.includes('root=True'))
        throw new Error('C# runtime invariants failed');
    status.phase = 'managed-ready'; status.startupMs = Date.now() - started;
    console.log('[he-csharp-probe]', JSON.stringify(status)); draw();
    wx.onTouchStart(() => { status.count = probe.Step(7); status.calls++; draw(); console.log('[he-csharp-probe-touch]', JSON.stringify(status)); });
})().catch(fail);
