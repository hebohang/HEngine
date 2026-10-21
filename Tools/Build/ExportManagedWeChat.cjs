'use strict';
// Reuse the real .NET Wasm binaries; adapt only the pinned loader's platform I/O.
const fs=require('node:fs'),path=require('node:path');
const esbuild=require('../Scripting/node_modules/esbuild');
const {validateManagedWeChat}=require('./ManagedWeChatCompatibility.cjs');
const {compileWeChatJavaScript}=require('./WeChatJavaScript.cjs');
function exportManaged(input,output) {
    const read=name=>fs.readFileSync(path.join(input,name));
    const add=(name,data)=>{const file=path.join(output,name);fs.mkdirSync(path.dirname(file),{recursive:true});fs.writeFileSync(file,name.endsWith('.js')?compileWeChatJavaScript(data,name):data);};
    const loader=read('dotnet.js').toString(),match=loader.match(/\/\*json-start\*\/([\s\S]*?)\/\*json-end\*\//);
    if(!match)throw Error('Pinned .NET boot manifest format changed');
    const config=JSON.parse(match[1]);
    const compatibility=validateManagedWeChat(config,read);
    for(const flag of ['--no-jiterpreter-traces-enabled','--no-jiterpreter-interp-entry-enabled','--no-jiterpreter-jit-call-enabled'])
        if(!config.runtimeOptions.includes(flag))throw Error('Dynamic Wasm must be disabled: '+flag);
    const assets=Object.values(config.resources).filter(Array.isArray).flat(),modules=['dotnet.js',...assets.filter(a=>a.name.endsWith('.js')).map(a=>a.name)];
    const probes=[...loader.matchAll(/WebAssembly\.validate\(new Uint8Array\(\[([0-9, ]+)\]\)\)/g)],features={};
    if(probes.length!==3)throw Error('Pinned .NET feature probes changed');
    for(const [i,probe] of probes.entries()) {
        const bytes=Buffer.from(probe[1].split(',').map(Number)),name=`runtime/feature-${i}.wasm`;
        add(name,bytes);features[[...bytes].join(',')]=name;
    }
    for(const name of modules) {
        const code=read(name).toString().replace(/import\.meta\.url/g,JSON.stringify('https://he.invalid/runtime/'+name)).replace(/\bimport\s*\(/g,'__heImport(');
        const imports='function __heImport(url){switch(String(url).split("/").pop()){'+modules.map(m=>`case ${JSON.stringify(m)}:return Promise.resolve(require(${JSON.stringify('./'+m)}));`).join('')+'default:return Promise.reject(Error("Unknown Mono module: "+url));}}\n';
        add('runtime/'+name,"let globalThis=require('../runtime-env.js');const WebAssembly=globalThis.WebAssembly;const performance=globalThis.performance;\n"+imports+esbuild.transformSync(code,{format:'cjs',target:'es2019',sourcefile:name}).code);
    }
    for(const asset of assets.filter(a=>!a.name.endsWith('.js')))add('runtime/'+asset.name+(config.resources.wasmNative.some(a=>a.name===asset.name)?'':'.bin'),read(asset.name));
    add('runtime/game.js','module.exports={};');
    add('runtime-env.js','module.exports={};');
    add('host-clock.js',fs.readFileSync(path.join(__dirname,'../../Engine/Source/Runtime/Platform/WeChat/Clock.js')));
    add('runtime-features.js','module.exports='+JSON.stringify(features)+';');
    add('runtime-config.js','module.exports='+JSON.stringify(config)+';');
    add('runtime-compatibility.json',JSON.stringify(compatibility,null,2));
    add('wechat-runtime.js',fs.readFileSync(path.join(__dirname,'../../Engine/Source/Runtime/Platform/WeChat/ManagedRuntime.cjs')));
    return config;
}
module.exports={exportManaged};
