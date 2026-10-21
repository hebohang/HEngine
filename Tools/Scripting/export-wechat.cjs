'use strict';
// Adapt a verified Web build: the engine and C# binaries are reused byte-for-byte.
const fs = require('node:fs'), path = require('node:path'), crypto = require('node:crypto'), esbuild = require('esbuild');
const root = path.resolve(__dirname, '../..'), input = path.resolve(process.argv[2] || path.join(root,'out/agent/scripting/game-web'));
const output = path.resolve(process.argv[3] || path.join(root,'out/agent/scripting/game-wechat'));
const source = JSON.parse(fs.readFileSync(path.join(input,'game-manifest.json')));
if (!source.registration || source.registration.identity !== source.managedIdentity ||
    source.registration.gameplaySourceIdentity !== source.gameplaySourceIdentity)
    throw new Error('Missing matching generated registration; rebuild the portable package.');
const hash = bytes => crypto.createHash('sha256').update(bytes).digest('hex');
const files = new Map();
function safe(base, name) {
    const target = path.resolve(base,name);
    if (!target.startsWith(base+path.sep) || name.includes('\\')) throw new Error('Unsafe package path: '+name);
    return target;
}
function add(name, bytes) { safe(output,name); files.set(name,Buffer.from(bytes)); }
function read(name) {
    const item = source.assets.find(a => a.path === name);
    if (!item) throw new Error('Missing Web build input: '+name);
    const bytes = fs.readFileSync(safe(input,name));
    if (hash(bytes) !== item.sha256) throw new Error('Modified Web build input: '+name);
    return bytes;
}
const loader = read('_framework/dotnet.js').toString();
const match = loader.match(/\/\*json-start\*\/([\s\S]*?)\/\*json-end\*\//);
if (!match) throw new Error('Pinned Mono boot manifest format changed');
const config = JSON.parse(match[1]);
const compatibility = require('../Build/ManagedWeChatCompatibility.cjs').validateManagedWeChat(config, name=>read('_framework/'+name));
add('runtime-compatibility.json',JSON.stringify(compatibility,null,2));
for (const flag of ['--no-jiterpreter-traces-enabled','--no-jiterpreter-interp-entry-enabled','--no-jiterpreter-jit-call-enabled'])
    if (!config.runtimeOptions.includes(flag)) throw new Error('Dynamic Wasm generation must be disabled: '+flag);
const assets = Object.values(config.resources).filter(Array.isArray).flat();
const modules = ['dotnet.js',...assets.filter(a=>a.name.endsWith('.js')).map(a=>a.name)];
const featureFiles = {}, probes = [...loader.matchAll(/WebAssembly\.validate\(new Uint8Array\(\[([0-9, ]+)\]\)\)/g)];
if (probes.length !== 3) throw new Error('Pinned runtime feature probes changed');
for (const [index,probe] of probes.entries()) {
    const bytes = Buffer.from(probe[1].split(',').map(Number)), name = `runtime/feature-${index}.wasm`;
    add(name,bytes); featureFiles[[...bytes].join(',')] = name;
}
for (const name of modules) {
    const code = read('_framework/'+name).toString().replace(/import\.meta\.url/g,JSON.stringify('https://he.invalid/runtime/'+name)).replace(/\bimport\s*\(/g,'__heImport(');
    const imports = 'function __heImport(url) {switch(String(url).split("/").pop()) {'+
        modules.map(m=>`case ${JSON.stringify(m)}: return Promise.resolve(require(${JSON.stringify('./'+m)}));`).join('\n')+
        'default: return Promise.reject(new Error("Unknown Mono module: "+url));}}\n';
    add('runtime/'+name,"let globalThis = require('../runtime-env.js');\nconst WebAssembly = globalThis.WebAssembly;const performance=globalThis.performance;\n"+imports+
        esbuild.transformSync(code,{format:'cjs',target:'es2020',sourcefile:name}).code);
}
for (const asset of assets.filter(a=>!a.name.endsWith('.js'))) {
    const bytes = read('_framework/'+asset.name);
    const nativeWasm = config.resources.wasmNative.some(a=>a.name===asset.name);
    add('runtime/'+asset.name+(nativeWasm?'':'.bin'),bytes);
}
add('runtime-config.js','module.exports = '+JSON.stringify(config)+';\n');
add('runtime-features.js','module.exports = '+JSON.stringify(featureFiles)+';\n');
add('runtime-env.js','module.exports = {};\n');
add('host-clock.js',fs.readFileSync(path.join(root,'Engine/Source/Runtime/Platform/WeChat/Clock.js')));
for (const name of ['hegame.js','hegame.wasm']) add('engine/'+name,read(name));
const project = JSON.parse(fs.readFileSync(path.join(root,'Project/Config/WeChat.json')));
const resources = [];
for (const asset of source.assets.filter(a=>a.path.startsWith('project/'))) {
    const name = asset.path.slice(8), packagePath = 'resources/'+name+'.bin';
    add(packagePath,read(asset.path)); resources.push({path:name,packagePath,bytes:asset.bytes});
}
add(project.audio,fs.readFileSync(safe(path.join(root,'Project'),project.audio)));
for (const name of ['host.js','boot-diagnostics.js']) add(name,fs.readFileSync(path.join(root,'Engine/Source/Runtime/Platform/WeChat',name)));
add('game.js',fs.readFileSync(path.join(__dirname,'Portable/wechat-game.js')));
add('wechat-runtime.js',fs.readFileSync(path.join(root,'Engine/Source/Runtime/Platform/WeChat/ManagedRuntime.cjs')));
add('engine-transport.js',"const performance=require('./host-clock.js').createClock(globalThis);\n"+esbuild.transformSync(read('engine-transport.js').toString(),{format:'cjs',target:'es2020'}).code);
for (const asset of source.assets.filter(a=>a.path.startsWith('licenses/'))) add(asset.path,read(asset.path));
add('licenses/esbuild-MIT.md',fs.readFileSync(path.join(__dirname,'node_modules/esbuild/LICENSE.md')));
// Mono is an ordinary subpackage; the 4 MiB main package contains the real engine.
add('runtime/game.js','module.exports = {};\n');
add('game.json',JSON.stringify({deviceOrientation:project.orientation,subpackages:[{name:'managed',root:'runtime/'}]}));
add('project.config.json',JSON.stringify({appid:project.appid,projectname:'HEngine CSharp Game',compileType:'game',
    setting:{es6:false,minified:false,urlCheck:false},libVersion:'3.17.2'},null,2));
// The upload validator rejects optional chaining despite DevTools executing it.
// Lower host syntax only; both Wasm binaries and managed assemblies are untouched.
for (const [name,bytes] of files) if (name.endsWith('.js'))
    files.set(name,Buffer.from(require('../Build/WeChatJavaScript.cjs').compileWeChatJavaScript(bytes,name)));
const buildId = hash(JSON.stringify([...files].map(([name,bytes])=>[name,hash(bytes)])));
add('he-config.js','module.exports = '+JSON.stringify({...project,scene:source.scene,texture:source.texture,
    storageKey:'he-wechat-csharp-v1',resources,buildId,gameplaySourceIdentity:source.gameplaySourceIdentity,managedIdentity:source.managedIdentity,registration:source.registration,api:source.api})+';\n');
const packageBytes = {main:0,managed:0};
for (const [name,bytes] of files) packageBytes[name.startsWith('runtime/')?'managed':'main'] += bytes.length;
if (packageBytes.main > 4*1024*1024) throw new Error('WeChat main package exceeds 4 MiB');
if (packageBytes.main+packageBytes.managed > 20*1024*1024) throw new Error('WeChat total package exceeds 20 MiB');
const manifestPath = path.join(output,'game-manifest.json');
const previous = fs.existsSync(manifestPath) ? JSON.parse(fs.readFileSync(manifestPath)) : {assets:[]};
const previousNames = new Set(previous.assets.map(a=>a.path));
for (const asset of previous.assets) {
    const target = safe(output,asset.path);
    if (fs.existsSync(target) && hash(fs.readFileSync(target)) !== asset.sha256) throw new Error('Modified export file: '+asset.path);
}
for (const name of files.keys()) if (fs.existsSync(safe(output,name)) && !previousNames.has(name)) throw new Error('Unowned export file: '+name);
for (const [name,bytes] of files) { const target=safe(output,name); fs.mkdirSync(path.dirname(target),{recursive:true}); fs.writeFileSync(target,bytes); }
for (const asset of previous.assets) if (!files.has(asset.path) && fs.existsSync(safe(output,asset.path))) fs.unlinkSync(safe(output,asset.path));
const manifest = {schema:1,target:'WeChat',sdk:source.sdk,runtime:source.runtime,buildId,sourceWebBuildId:source.buildId,
    gameplaySourceIdentity:source.gameplaySourceIdentity,managedIdentity:source.managedIdentity,assets:[...files].map(([name,bytes])=>({path:name,bytes:bytes.length,sha256:hash(bytes)}))};
manifest.packageBytes = packageBytes;
fs.writeFileSync(manifestPath,JSON.stringify(manifest,null,2));
console.log(JSON.stringify({output,buildId,gameplaySourceIdentity:source.gameplaySourceIdentity,managedIdentity:source.managedIdentity,files:files.size,bytes:[...files.values()].reduce((n,b)=>n+b.length,0)}));
