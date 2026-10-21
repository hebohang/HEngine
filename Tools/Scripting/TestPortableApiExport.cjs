'use strict';
const fs = require('node:fs'), path = require('node:path'), assert = require('node:assert/strict');
const {spawnSync} = require('node:child_process');
const root = path.resolve(__dirname, '../..');
const registry = path.resolve(process.argv[2]);
const work = path.resolve(process.argv[3]);
const metadata = JSON.parse(fs.readFileSync(path.join(registry, 'GameScripts.metadata.json')));
assert(metadata.types.find(t => t.id === 'sample.touch').portableDiagnostics.some(d => d.includes('System.IO.File.ReadAllText')));
fs.mkdirSync(work, {recursive:true});
const results = [];
for (const scenario of ['unsupported-call', 'missing-audit', 'outdated-audit']) {
    const input = structuredClone(metadata);
    if (scenario === 'missing-audit') delete input.types.find(t => t.id === 'sample.touch').portableApiContract;
    if (scenario === 'outdated-audit') {
        const type = input.types.find(t => t.id === 'sample.touch');
        type.portableApiContract=1; type.portableDiagnostics=[];
    }
    const directory = path.join(work, scenario);
    fs.mkdirSync(directory, {recursive:true});
    fs.writeFileSync(path.join(directory, 'GameScripts.metadata.json'), JSON.stringify(input));
    const output = path.join(directory, 'export');
    const result = spawnSync(process.execPath, [path.join(__dirname, 'export-web.cjs'),
        path.join(root, 'out/agent/scripting/game-web-managed'), path.join(root, 'out/agent/web-script-release'), directory, output],
        {encoding:'utf8', timeout:30000});
    fs.writeFileSync(path.join(directory, 'export.log'), (result.stdout || '') + (result.stderr || ''));
    assert.notEqual(result.status, 0);
    assert.match(result.stderr, /Assets\/WeChatSample\/ScriptTouch.he: entity .*script sample.touch/);
    assert.match(result.stderr, scenario === 'unsupported-call' ? /Helper.Read.*System.IO.File.ReadAllText/ : /Missing current portable API audit/);
    assert(!fs.existsSync(output), 'Rejected export must not publish any output');
    results.push({scenario, status:'PASS', exitCode:result.status});
}
fs.writeFileSync(path.join(work, 'results.json'), JSON.stringify(results, null, 2));
console.log('Portable API real exporter rejection PASS: ' + work);
