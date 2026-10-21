'use strict';
// Publish the real, separately linked engine and Mono memories. No gameplay translation.
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const root = path.resolve(__dirname, '../..');
const [managedArg, nativeArg, registryArg, outputArg] = process.argv.slice(2);
if (!outputArg) throw new Error('Usage: node export-web.cjs <managed publish> <native build> <registry> <output>');
const managed = path.resolve(managedArg, 'wwwroot/_framework'), native = path.resolve(nativeArg);
const output = path.resolve(outputArg), registry = JSON.parse(fs.readFileSync(path.resolve(registryArg, 'GameScripts.metadata.json')));
const hash = bytes => crypto.createHash('sha256').update(bytes).digest('hex');
const safe = (base, name) => {
    const target = path.resolve(base, name);
    if (!target.startsWith(base + path.sep) || name.includes('\\')) throw new Error('Unsafe package path: ' + name);
    return target;
};
const files = new Map();
function add(name, bytes) { safe(output, name); files.set(name, Buffer.isBuffer(bytes) ? bytes : Buffer.from(bytes)); }
function copy(name, source) { add(name, fs.readFileSync(source)); }
const loader = fs.readFileSync(path.join(managed, 'dotnet.js'), 'utf8');
const match = loader.match(/\/\*json-start\*\/([\s\S]*?)\/\*json-end\*\//);
if (!match) throw new Error('Pinned .NET boot manifest format changed');
const config = JSON.parse(match[1]);
for (const flag of ['--no-jiterpreter-traces-enabled', '--no-jiterpreter-interp-entry-enabled', '--no-jiterpreter-jit-call-enabled'])
    if (!config.runtimeOptions.includes(flag)) throw new Error('Required interpreter configuration missing: ' + flag);
copy('_framework/dotnet.js', path.join(managed, 'dotnet.js'));
for (const asset of Object.values(config.resources).filter(Array.isArray).flat()) {
    const bytes = fs.readFileSync(safe(managed, asset.name));
    if (asset.hash && 'sha256-' + crypto.createHash('sha256').update(bytes).digest('base64') !== asset.hash)
        throw new Error('Published .NET asset integrity mismatch: ' + asset.name);
    add('_framework/' + asset.name, bytes);
}
const sceneName = 'Assets/WeChatSample/ScriptTouch.he', texture = 'Assets/WeChatSample/checker.png';
const scene = JSON.parse(fs.readFileSync(path.join(root, 'Project', sceneName)));
const resources = new Set([sceneName, texture]);
const components = new Set(['TagComponent', 'TransformComponent', 'SpriteRendererComponent', 'CameraComponent', 'ScriptComponent']);
for (const [entity, values] of Object.entries(scene)) {
    const fail = reason => { throw new Error(`${sceneName}: entity ${entity}, script ${values.ScriptComponent?.Type || '(none)'}: ${reason}`); };
    for (const name of Object.keys(values)) if (!components.has(name)) fail('Web sample does not support component ' + name);
    if (values.SpriteRendererComponent?.Texture) resources.add(values.SpriteRendererComponent.Texture);
    const script = values.ScriptComponent;
    if (!script) continue;
    if (script.Backend !== 'he.script.csharp') fail('Backend is unsupported on Web: ' + script.Backend);
    const type = registry.types.find(type => type.id === script.Type);
    if (!type) fail('Type is missing from the generated C# registry');
    if (type.portableApiContract !== 2 || !Array.isArray(type.portableDiagnostics)) fail('Missing current portable API audit; rebuild C# scripts');
    if (type.portableDiagnostics.length) fail('Unsupported portable API: ' + type.portableDiagnostics.join('; '));
    const fields = JSON.parse(script.Fields).fields;
    for (const field of type.fields) {
        const value = fields[field.id] || field.formerIds.map(id => fields[id]).find(Boolean) || field.defaultValue;
        if (value.type !== field.defaultValue.type) fail('Field type mismatch: ' + field.id);
        if (value.type === 'resource' && value.value) resources.add(value.value);
    }
    for (const dependency of type.dependencies) resources.add(dependency);
}
for (const resource of resources) copy('project/' + resource, safe(path.join(root, 'Project'), resource));
for (const name of ['hegame.js', 'hegame.wasm']) copy(name, path.join(native, name));
for (const name of ['main.js', 'index.html', 'engine-transport.js']) copy(name, path.join(__dirname, 'Portable', name));
const runtimePack = process.env.HE_SCRIPT_RUNTIME_PACK || 'C:/Program Files/dotnet/packs/Microsoft.NETCore.App.Runtime.Mono.browser-wasm/10.0.10';
for (const name of ['LICENSE.TXT', 'THIRD-PARTY-NOTICES.TXT']) copy('licenses/dotnet-' + name, path.join(runtimePack, name));
const notices = {
    'sokol.txt':'out/agent/wechat/deps/sokol-LICENSE',
    'emscripten.txt':'out/agent/wechat/deps/emsdk/upstream/install/emscripten/LICENSE',
    'entt.txt':'Engine/Source/ThirdParty/entt/LICENSE', 'glm.txt':'Engine/Source/ThirdParty/glm/copying.txt',
    'json.txt':'Engine/Source/ThirdParty/json/LICENSE.MIT', 'spdlog.txt':'Engine/Source/ThirdParty/spdlog/LICENSE',
    'crossguid.txt':'Engine/Source/ThirdParty/crossguid/LICENSE'
};
for (const [name, source] of Object.entries(notices)) copy('licenses/' + name, path.join(root, source));
const stb = fs.readFileSync(path.join(root,'Engine/Source/ThirdParty/stb_image/stb_image.h'),'utf8');
const licenseStart = stb.lastIndexOf('ALTERNATIVE A - MIT License');
if (licenseStart < 0) throw new Error('stb_image distribution notice is missing');
add('licenses/stb_image.txt',stb.slice(licenseStart));
const assets = [...files].map(([name, bytes]) => ({path:name, sha256:hash(bytes), bytes:bytes.length}));
const manifest = {schema:1, sdk:'10.0.302', runtime:'10.0.10', target:'Web', scene:sceneName, texture,
    registration:registry,
    gameplaySourceIdentity:registry.gameplaySourceIdentity, managedIdentity:registry.identity, buildId:hash(JSON.stringify(assets)),
    api:JSON.parse(fs.readFileSync(path.join(__dirname, 'Api.schema.json'))), assets};
const manifestPath = path.join(output, 'game-manifest.json');
const previous = fs.existsSync(manifestPath) ? JSON.parse(fs.readFileSync(manifestPath)) : {assets:[]};
const previousNames = new Set(previous.assets.map(asset => asset.path));
// Refuse to overwrite unknown or locally edited output. Delete only unchanged prior owned files.
for (const asset of previous.assets) {
    const target = safe(output, asset.path);
    if (fs.existsSync(target) && hash(fs.readFileSync(target)) !== asset.sha256) throw new Error('Export contains modified file: ' + asset.path);
}
for (const name of files.keys()) if (fs.existsSync(safe(output, name)) && !previousNames.has(name)) throw new Error('Export contains unowned file: ' + name);
for (const [name, bytes] of files) { const target = safe(output, name); fs.mkdirSync(path.dirname(target), {recursive:true}); fs.writeFileSync(target, bytes); }
for (const asset of previous.assets) if (!files.has(asset.path) && fs.existsSync(safe(output, asset.path))) fs.unlinkSync(safe(output, asset.path));
fs.writeFileSync(manifestPath, JSON.stringify(manifest, null, 2));
console.log(JSON.stringify({output, buildId:manifest.buildId, managedIdentity:registry.identity, files:assets.length, bytes:assets.reduce((sum,a) => sum+a.bytes,0)}));
