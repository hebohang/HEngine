'use strict';
// Compare verified export contents, not cache sizes or compressed downloads.
const fs = require('node:fs'), path = require('node:path'), crypto = require('node:crypto');
const root = path.resolve(__dirname, '../..');
function measure(directory, manifestName) {
    const manifest = JSON.parse(fs.readFileSync(path.join(directory, manifestName), 'utf8'));
    const entries = manifest.assets || Object.entries(manifest.files).map(([name, entry]) => ({path:name, ...entry}));
    const ignored = new Set();
    const configPath = path.join(directory, 'project.config.json');
    if (fs.existsSync(configPath)) {
        const config = JSON.parse(fs.readFileSync(configPath));
        for (const item of config.packOptions?.ignore || []) {
            if (item.type !== 'file') throw new Error('Unsupported package exclusion: '+item.type);
            ignored.add(item.value);
        }
    }
    let declaredBytes = 0, excludedBytes = 0, runtimeBytes = 0, engineBytes = 0;
    for (const entry of entries) {
        const file = path.resolve(directory, entry.path);
        if (!file.startsWith(directory + path.sep)) throw new Error('Manifest path escape');
        const bytes = fs.readFileSync(file);
        if (bytes.length !== entry.bytes || crypto.createHash('sha256').update(bytes).digest('hex') !== entry.sha256)
            throw new Error('Changed export artifact: '+entry.path);
        declaredBytes += bytes.length;
        if (ignored.has(entry.path)) excludedBytes += bytes.length;
        if (entry.path.startsWith('runtime/') || entry.path.startsWith('_framework/')) runtimeBytes += bytes.length;
        if (/^(engine\/)?hegame\.(js|wasm)$/.test(entry.path)) engineBytes += bytes.length;
    }
    return {directory:path.relative(root,directory), buildId:manifest.buildId || null,
        manifestSha256:crypto.createHash('sha256').update(fs.readFileSync(path.join(directory,manifestName))).digest('hex'),
        declaredBytes, excludedBytes, eligibleDeclaredBytes:declaredBytes-excludedBytes, runtimeBytes, engineBytes};
}
const native = measure(path.join(root,'out/agent/wechat-export'), 'resource-manifest.json');
const receipt = JSON.parse(fs.readFileSync(path.join(root,'out/agent/wechat-wasm-release/build-receipt.json')));
const nativeManifest = JSON.parse(fs.readFileSync(path.join(root,'out/agent/wechat-export/resource-manifest.json')));
for (const name of ['hegame.js','hegame.wasm']) {
    if (nativeManifest.files['engine/'+name]?.sha256 !== receipt.artifacts[name])
        throw new Error('Native export does not match the latest successful build: '+name);
}
const csharp = measure(path.join(root,'out/agent/scripting/game-wechat'), 'game-manifest.json');
const web = measure(path.join(root,'out/agent/scripting/game-web'), 'game-manifest.json');
const nativeWeb = process.argv[3] ? measure(path.resolve(process.argv[3]), 'game-manifest.json') : null;
const result = {evidenceType:'verified-export-size', nativeWeChat:native, csharpWeChat:csharp, csharpWeb:web,
    nativeWeb, webDeltaBytes:nativeWeb ? web.eligibleDeclaredBytes-nativeWeb.eligibleDeclaredBytes : null,
    wechatDeltaBytes:csharp.eligibleDeclaredBytes-native.eligibleDeclaredBytes,
    limitations:['Uncompressed declared files; excludes each manifest itself and undeclared local files.',
        'WeChat ignores browser diagnostics as specified by project.config.json.',
        'Native TouchSample and C# sample differ in scene, host and gameplay; delta includes those differences.',
        nativeWeb ? 'Web delta compares the original C++ TouchSample browser adapter with the C# sample, including different scenes and hosts. No device measurement is claimed.' :
            'Web has no native baseline supplied; no Web delta or device measurement is claimed.']};
const output = path.resolve(process.argv[2] || path.join(root,'out/agent/scripting/portable-sizes.json'));
fs.writeFileSync(output,JSON.stringify(result,null,2)+'\n');
console.log(JSON.stringify(result,null,2));
