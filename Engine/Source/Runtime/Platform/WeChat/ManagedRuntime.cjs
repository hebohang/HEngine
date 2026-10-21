'use strict';
// Shared WeChat managed host. The pinned Mono loader owns an isolated environment.
async function createManagedRuntime(config) {

// Keep .NET host shims local to its transformed modules. The C++ engine host
// must retain its own fetch/WebAssembly bindings and independent memory.
const runtimeEnvironment = require('./runtime-env.js');
runtimeEnvironment.performance = require('./host-clock.js').createClock(globalThis);
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
        filePath: packageName(url) + (String(url).split('/').pop() === config.resources.wasmNative[0].name ? '' : '.bin'),
        success: result => resolve(result.data), fail: reject
    }));
    packageBuffers.set(data, packageName(url));
    return { ok: true, status: 200, url: String(url), headers: { get: () => null },
        arrayBuffer: async () => data, json: async () => { throw new Error('JSON asset request unsupported'); } };
};
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
        try {
            if(typeof WXWebAssembly.compile==='function')await WXWebAssembly.compile(file);
            else await WXWebAssembly.instantiate(file,{});
            supportedFeatures.set(bytes, true);
        }
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
            const module = typeof WXWebAssembly.compile==='function'?await WXWebAssembly.compile(file):new WXWebAssembly.Module(file);
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

            return result.instance || result;
        }
    });

    return await require('./runtime/dotnet.js').dotnet.withModuleConfig({
        print: text => console.info('[HE C#]', text), printErr: text => console.error('[HE C#]', text)
    }).withDiagnosticTracing(false).create();
}
module.exports = {createManagedRuntime};
