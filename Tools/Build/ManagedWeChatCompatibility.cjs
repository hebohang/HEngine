'use strict';
// Inspect the actual native loader, not project intent or runtimeBuildInfo
// (the latter describes the prebuilt managed support library).
function validateManagedWeChat(config, read) {
    const native = config.resources?.jsModuleNative;
    if (!Array.isArray(native) || native.length !== 1) throw Error('Unknown .NET native loader manifest. Revalidate the WeChat exporter for this SDK.');
    const code = read(native[0].name).toString();
    const setup = code.match(/\bDOTNET\.setup\(\s*\{([^}]+)\}/);
    if (!setup) throw Error('Unknown .NET native build options. Revalidate the WeChat exporter for this SDK.');
    for (const [flag, property] of [['wasmEnableEH','WasmEnableExceptionHandling'],['wasmEnableSIMD','WasmEnableSIMD']]) {
        const value = setup[1].match(new RegExp('\\b'+flag+'\\s*:\\s*(false|true|0|1|!0|!1)(?=\\s*[,}])'));
        if (!value || !['false','0','!1'].includes(value[1]))
            throw Error(`WeChat mobile compatibility: rebuild with ${property}=false and WasmBuildNative=true. Desktop DevTools support is not proof of phone compatibility.`);
    }
    if (config.resources.jsModuleWorker?.length) throw Error('WeChat mobile compatibility: rebuild with WasmEnableThreads=false.');
    return {schema:1, exceptionHandling:'javascript', simd:false, threads:false};
}
module.exports={validateManagedWeChat};
if (require.main===module) {
    const fs=require('node:fs'),path=require('node:path'),root=path.resolve(process.argv[2]);
    const read=name=>fs.readFileSync(path.join(root,name));
    const manifest=read('dotnet.js').toString().match(/\/\*json-start\*\/([\s\S]*?)\/\*json-end\*\//);
    if(!manifest)throw Error('Unknown .NET boot manifest.');
    validateManagedWeChat(JSON.parse(manifest[1]),read);
    console.log('PASS: managed WeChat mobile compatibility (JavaScript EH, no SIMD or threads)');
}
