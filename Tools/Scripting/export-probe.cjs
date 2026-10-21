'use strict';
// Adapt only the pinned .NET JS module loader. No C# gameplay is translated.
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const esbuild = require('esbuild');
const repository = path.resolve(__dirname, '../..');
const input = path.join(repository, 'out/agent/scripting/probe-web/wwwroot/_framework');
const output = path.join(repository, 'out/agent/scripting/probe-wechat');
const loader = fs.readFileSync(path.join(input, 'dotnet.js'), 'utf8');
const start = loader.indexOf('/*json-start*/');
const end = loader.indexOf('/*json-end*/');
if (start < 0 || end < start) throw new Error('Pinned .NET boot manifest format changed');
const config = JSON.parse(loader.slice(start + 14, end));
for (const flag of ['--no-jiterpreter-traces-enabled', '--no-jiterpreter-interp-entry-enabled', '--no-jiterpreter-jit-call-enabled'])
    if (!config.runtimeOptions.includes(flag)) throw new Error('Dynamic Jiterpreter must be disabled: ' + flag);
const assets = Object.values(config.resources).filter(Array.isArray).flat();
const modules = ['dotnet.js', ...assets.filter(a => a.name.endsWith('.js')).map(a => a.name)];
const runtimePath = path.join(output, 'runtime');
fs.mkdirSync(runtimePath, { recursive: true });
fs.writeFileSync(path.join(output, 'runtime-env.js'), 'module.exports = {};\n');
const previousManifestPath = path.join(output, 'probe-manifest.json');
const previous = fs.existsSync(previousManifestPath) ? JSON.parse(fs.readFileSync(previousManifestPath, 'utf8')) : { assets: [] };
const manifest = [];
const hash = bytes => crypto.createHash('sha256').update(bytes).digest('hex');
const featureFiles = {};
const featureMatches = [...loader.matchAll(/WebAssembly\.validate\(new Uint8Array\(\[([0-9, ]+)\]\)\)/g)];
if (featureMatches.length !== 3) throw new Error('Pinned .NET feature probes changed; review host adapter');
for (const [index, match] of featureMatches.entries()) {
    const bytes = Buffer.from(match[1].split(',').map(Number));
    const name = 'runtime/feature-' + index + '.wasm';
    fs.writeFileSync(path.join(output, name), bytes);
    featureFiles[[...bytes].join(',')] = name;
    manifest.push({ path: name, sha256: hash(bytes), bytes: bytes.length });
}
fs.writeFileSync(path.join(output, 'runtime-features.js'), 'module.exports = ' + JSON.stringify(featureFiles) + ';\n');
for (const name of modules) {
    const raw = fs.readFileSync(path.join(input, name), 'utf8');
    const source = raw.replace(/import\.meta\.url/g, JSON.stringify('https://he.invalid/runtime/' + name))
        .replace(/\bimport\s*\(/g, '__heImport(');
    const transformed = esbuild.transformSync(source, { format: 'cjs', target: 'es2020', sourcefile: name }).code;
    const imports = `function __heImport(url) { const name = String(url).split('/').pop(); switch(name) {\n` +
        modules.map(m => `case ${JSON.stringify(m)}: return Promise.resolve(require(${JSON.stringify('./' + m)}));`).join('\n') +
        `\ndefault: return Promise.reject(new Error('Unsupported .NET module import: ' + url)); }}\n`;
    const environment = "const globalThis = require('../runtime-env.js');\nconst WebAssembly = globalThis.WebAssembly;\n";
    const bytes = environment + imports + transformed;
    fs.writeFileSync(path.join(runtimePath, name), bytes);
    manifest.push({ path: 'runtime/' + name, sourceSha256: hash(raw), sha256: hash(bytes), bytes: Buffer.byteLength(bytes) });
}
for (const asset of assets.filter(a => !a.name.endsWith('.js'))) {
    if (path.basename(asset.name) !== asset.name) throw new Error('Unsafe asset path');
    const bytes = fs.readFileSync(path.join(input, asset.name));
    if (asset.hash && crypto.createHash('sha256').update(bytes).digest('base64') !== asset.hash.slice(7))
        throw new Error('Published asset hash mismatch: ' + asset.name);
    // wx package readers do not accept every extension; use .bin for ICU data.
    fs.writeFileSync(path.join(runtimePath, asset.name + '.bin'), bytes);
    manifest.push({ path: 'runtime/' + asset.name + '.bin', sha256: hash(bytes), bytes: bytes.length });
    if (config.resources.wasmNative.some(a => a.name === asset.name)) {
        fs.writeFileSync(path.join(runtimePath, asset.name), bytes);
        manifest.push({ path: 'runtime/' + asset.name, sha256: hash(bytes), bytes: bytes.length });
    }
}
const projectConfig = JSON.parse(fs.readFileSync(path.join(repository, 'Project/Config/WeChat.json'), 'utf8'));
fs.writeFileSync(path.join(output, 'project.config.json'), JSON.stringify({
    appid: projectConfig.appid, projectname: 'HEngine CSharp Runtime Probe', compileType: 'game',
    setting: { es6: false, minified: false, urlCheck: false }, libVersion: 'latest'
}, null, 2));
fs.writeFileSync(path.join(output, 'game.json'), JSON.stringify({ deviceOrientation: 'portrait' }));
fs.writeFileSync(path.join(output, 'runtime-config.js'), 'module.exports = ' + JSON.stringify(config) + ';\n');
fs.copyFileSync(path.join(__dirname, 'probe-wechat.js'), path.join(output, 'game.js'));
if (process.env.HE_SCRIPT_RUNTIME_PACK) {
    fs.mkdirSync(path.join(output, 'licenses'), { recursive: true });
    for (const name of ['LICENSE.TXT', 'THIRD-PARTY-NOTICES.TXT'])
        fs.copyFileSync(path.join(process.env.HE_SCRIPT_RUNTIME_PACK, name), path.join(output, 'licenses', 'dotnet-' + name));
    fs.copyFileSync(path.join(__dirname, 'node_modules/esbuild/LICENSE.md'), path.join(output, 'licenses/esbuild-MIT.md'));
}
const supportFiles = ['game.js', 'game.json', 'project.config.json', 'runtime-config.js', 'runtime-features.js', 'runtime-env.js'];
if (process.env.HE_SCRIPT_RUNTIME_PACK)
    supportFiles.push('licenses/dotnet-LICENSE.TXT', 'licenses/dotnet-THIRD-PARTY-NOTICES.TXT', 'licenses/esbuild-MIT.md');
for (const name of supportFiles) {
    const bytes = fs.readFileSync(path.join(output, name));
    manifest.push({ path: name, sha256: hash(bytes), bytes: bytes.length });
}
// Prune only files previously recorded by this exporter within its fixed output.
const currentNames = new Set(manifest.map(a => a.path));
for (const old of previous.assets) {
    if (currentNames.has(old.path)) continue;
    const target = path.resolve(output, old.path);
    if (!target.startsWith(output + path.sep)) throw new Error('Unsafe previous manifest path');
    if (fs.existsSync(target)) fs.unlinkSync(target);
}
fs.writeFileSync(path.join(output, 'probe-manifest.json'), JSON.stringify({ sdk: '10.0.302', runtime: '10.0.10', assets: manifest }, null, 2));
console.log(JSON.stringify({ output, files: manifest.length, bytes: manifest.reduce((sum, a) => sum + a.bytes, 0) }));
