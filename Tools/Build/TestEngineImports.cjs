'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const childProcess = require('node:child_process');
const esbuild = require('../Scripting/node_modules/esbuild');
const {engineAliases, engineSiteImports} = require('./EngineImports.cjs');

async function main() {
const root = path.resolve(__dirname, '../..');
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'HEngine independent project '));
try {
    // Deliberately outside the engine, with no sibling Engine/ or Tools/.
    for (const directory of ['WeChat', 'Web']) {
        fs.cpSync(path.join(root, 'Project', directory), path.join(temporary, directory), {recursive: true});
    }
    const result = await esbuild.build({
        entryPoints: [path.join(temporary, 'WeChat/game.js')],
        bundle: true, write: false, format: 'cjs', target: 'es2019', metafile: true,
        alias: engineAliases(root),
        plugins: [engineSiteImports(root)],
        external: ['./he-game-config.js', './wechat-runtime.js', './runtime-config.js'],
    });
    assert.ok(result.outputFiles[0].contents.length > 0);
    assert.ok(Object.keys(result.metafile.inputs).some(name => name.includes('Platform/WeChat/Viewport.js')));
    assert.ok(!result.outputFiles[0].text.includes('@hengine/'));
    assert.ok(Object.keys(result.metafile.inputs).some(name => name.includes('Platform/Web/SceneRenderer.js')));

    // Test the project's launcher separately, against an engine in another
    // location. The stub checks argument forwarding, not real Wasm export.
    const fixtureEngine = path.join(temporary, 'selected engine');
    fs.mkdirSync(path.join(fixtureEngine, 'Tools/Build'), {recursive: true});
    fs.writeFileSync(path.join(fixtureEngine, 'Tools/Build/ExportWeChatGame.cjs'),
        'exports.exportWeChatGame=async options=>({input:options.input,managed:options.managed,output:options.output});');
    fs.mkdirSync(path.join(temporary, 'Tools'));
    fs.mkdirSync(path.join(temporary, 'Config'));
    fs.copyFileSync(path.join(root, 'Project/Tools/ExportWeChat.cjs'), path.join(temporary, 'Tools/ExportWeChat.cjs'));
    fs.writeFileSync(path.join(temporary, 'Config/WeChat.json'), '{}');
    const output = childProcess.execFileSync(process.execPath,
        [path.join(temporary, 'Tools/ExportWeChat.cjs'), 'content', 'managed', 'output'],
        {env: {...process.env, HE_ENGINE_ROOT: fixtureEngine}, encoding: 'utf8'});
    assert.deepEqual(JSON.parse(output), {input: 'content', managed: 'managed', output: 'output'});
    console.log('PASS: relocated WeChat entry bundles; launcher uses selected engine (fixture, not device validation).');
} finally {
    fs.rmSync(temporary, {recursive: true, force: true});
}
}
main().catch(error => { console.error(error); process.exitCode = 1; });
