'use strict';
const fs = require('node:fs'), path = require('node:path'), crypto = require('node:crypto');
const root = path.resolve(__dirname,'../..');
const source = path.join(root,'out/agent/wechat-export');
const output = path.resolve(process.argv[2]);
const sha = bytes => crypto.createHash('sha256').update(bytes).digest('hex');
const manifest = JSON.parse(fs.readFileSync(path.join(source,'resource-manifest.json')));
const receipt = JSON.parse(fs.readFileSync(path.join(root,'out/agent/wechat-wasm-release/build-receipt.json')));
for (const [file, hash] of Object.entries(receipt.inputs)) {
    if (sha(fs.readFileSync(file)) !== hash) throw new Error('Native build input changed: '+file);
}
for (const name of ['hegame.js','hegame.wasm']) {
    if (manifest.files['engine/'+name]?.sha256 !== receipt.artifacts[name]) throw new Error('Native export is stale: '+name);
}
if (fs.existsSync(output)) throw new Error('Use a fresh baseline output directory.');
const assets = [];
const bytesByName = new Map();
for (const [name, entry] of Object.entries(manifest.files)) {
    const file = path.resolve(source,name);
    if (!file.startsWith(source+path.sep)) throw new Error('Manifest path escapes export.');
    const bytes = fs.readFileSync(file);
    if (bytes.length !== entry.bytes || sha(bytes) !== entry.sha256) throw new Error('Modified native artifact: '+name);
    // WeChat-only launch/configuration files have no role in this HTTP package.
    if (['game.js','game.json','project.config.json'].includes(name)) continue;
    bytesByName.set(name,bytes);
    assets.push({path:name,bytes:bytes.length,sha256:entry.sha256});
}
if (!bytesByName.has('browser-test.html') || !bytesByName.has('browser-harness.js')) throw new Error('Native browser entry missing.');
fs.mkdirSync(output,{recursive:true});
for (const [name,bytes] of bytesByName) {
    const file=path.join(output,name); fs.mkdirSync(path.dirname(file),{recursive:true}); fs.writeFileSync(file,bytes);
}
const result={schema:1,target:'Web',profile:'original-native-touch-browser-baseline',
    buildId:sha(Buffer.from(JSON.stringify(assets))),assets,
    declaredBytes:assets.reduce((sum,item)=>sum+item.bytes,0),
    entry:'browser-test.html',
    sourceManifestSha256:sha(fs.readFileSync(path.join(source,'resource-manifest.json'))),
    limitation:'Original C++ TouchSample browser adapter; gameplay and host differ from the C# sample. Uncompressed declared files, excluding this manifest.'};
fs.writeFileSync(path.join(output,'game-manifest.json'),JSON.stringify(result,null,2)+'\n');
console.log(JSON.stringify({output,buildId:result.buildId,declaredBytes:result.declaredBytes}));
