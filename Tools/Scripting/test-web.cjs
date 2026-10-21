'use strict';
// Headed browser integration evidence. Saved screenshots still require visual review.
const fs = require('node:fs'), path = require('node:path'), assert = require('node:assert/strict');
const {chromium} = require('playwright'), {PNG} = require('pngjs');
const url = process.argv[2] || 'http://127.0.0.1:8768';
const evidence = path.resolve(process.argv[3] || 'out/agent/scripting/web-evidence');
fs.mkdirSync(evidence, {recursive:true});
async function main() {
    const browser = await chromium.launch({headless:false, executablePath:process.env.HE_SCRIPT_CHROME || 'C:/Program Files/Google/Chrome/Application/chrome.exe'});
    const logs = [], errors = [], checks = [];
    let result = {status:'FAIL', evidenceType:'headed-browser-integration', visualReview:'PENDING'};
    try {
        const page = await browser.newPage({viewport:{width:1200,height:1000}, deviceScaleFactor:1});
        page.on('console', message => logs.push(message.type() + ': ' + message.text()));
        page.on('pageerror', error => errors.push(String(error.stack || error)));
        const ready = async () => { await page.waitForFunction(() => globalThis.HEGame?.running && HEGame.metrics.frames > 3, null, {timeout:30000}); };
        await page.goto(url); await ready();
        const save = () => page.evaluate(() => JSON.parse(HEGame.engine.UTF8ToString(HEGame.engine._he_save())));
        const fields = () => page.evaluate(() => HEGame.fields());
        const original = await save();
        assert.equal((await fields()).enableCount.value, 1); checks.push('fields-before-initialize');
        const canvas = page.locator('canvas'), initial = await canvas.screenshot({path:path.join(evidence,'initial.png')});
        const framebuffer = () => canvas.evaluate(async element => {
            const gl = element.getContext('webgl2'), bytes = new Uint8Array(element.width*element.height*4);
            gl.readPixels(0,0,element.width,element.height,gl.RGBA,gl.UNSIGNED_BYTE,bytes);
            if (gl.getError() !== gl.NO_ERROR) throw new Error('Game framebuffer readback failed');
            const hash = new Uint8Array(await crypto.subtle.digest('SHA-256',bytes));
            return {width:element.width,height:element.height,sha256:Array.from(hash,b=>b.toString(16).padStart(2,'0')).join('')};
        });
        const decoded = PNG.sync.read(initial), colors = new Set();
        for (let y=3;y<decoded.height-3;y++) for(let x=3;x<decoded.width-3;x++) colors.add(decoded.data.readUInt32BE((y*decoded.width+x)*4));
        assert(colors.size >= 3, 'Actual textured draw must differ from the clear color'); checks.push('textured-engine-draw-pixel-check');
        await page.keyboard.down('d'); await page.waitForTimeout(350); await page.keyboard.up('d');
        const moved = await save(); assert(moved.transform.Translation[0] > original.transform.Translation[0] + 0.2); checks.push('keyboard-movement');
        await canvas.click({position:{x:760,y:250}});
        await page.waitForFunction(() => HEGame.fields().score.value === 3);
        assert.equal(await page.evaluate(() => localStorage.getItem('HEngine.ScriptSample.v1.script-touch')), '3');
        await canvas.screenshot({path:path.join(evidence,'clicked.png')}); checks.push('pointer-score-and-save');
        await page.getByRole('button',{name:'禁用脚本',exact:true}).click();
        await page.waitForFunction(() => HEGame.entries.some(value => value.includes('Disable')));
        const disabled = await save(); await page.keyboard.down('d'); await page.waitForTimeout(150); await page.keyboard.up('d');
        assert.deepEqual((await save()).transform, disabled.transform);
        await page.getByRole('button',{name:'启用脚本',exact:true}).click();
        await page.waitForFunction(() => HEGame.fields().enableCount.value === 2); checks.push('disable-reenable');
        await page.getByRole('button',{name:'禁用脚本',exact:true}).click();
        await page.waitForFunction(() => HEGame.entries.filter(value => value.includes('Disable')).length >= 2);
        const persisted = await save();
        assert.equal(persisted.script.Enabled,false);
        assert.notDeepEqual(persisted.sprite.Color, original.sprite.Color, 'C# click must change the persisted sprite color');
        const beforeReload = await framebuffer();
        await canvas.screenshot({path:path.join(evidence,'before-reload.png')});
        await page.getByRole('button',{name:'保存并重新加载',exact:true}).click(); await page.waitForLoadState('load'); await ready();
        assert.equal((await fields()).score.value, 3); assert.deepEqual((await save()).transform,persisted.transform); checks.push('reload-field-transform-save');
        assert.deepEqual((await save()).sprite,persisted.sprite, 'Sprite state must survive scene reload');
        const afterReload = await framebuffer();
        await canvas.screenshot({path:path.join(evidence,'reloaded.png')});
        fs.writeFileSync(path.join(evidence,'reload-framebuffer.json'),JSON.stringify({before:beforeReload,after:afterReload},null,2));
        assert.deepEqual(afterReload,beforeReload, 'Game framebuffer changed after saving/reloading'); checks.push('reload-sprite-and-rendered-pixels');
        assert.equal((await save()).script.Enabled,false);
        await page.getByRole('button',{name:'启用脚本',exact:true}).click();
        await page.waitForFunction(() => HEGame.fields().enableCount.value === 3); checks.push('reload-disabled-lifecycle-and-ui');
        for(let i=0;i<20;i++) await page.getByRole('button',{name:'执行 GC',exact:true}).click();
        assert.equal((await fields()).score.value,3); checks.push('twenty-real-gc-root-survival');
        await page.getByRole('button',{name:'触发脚本异常',exact:true}).click();
        await page.waitForFunction(() => HEGame.entries.some(value => value.includes('InvalidOperationException')));
        const frame = await page.evaluate(() => HEGame.metrics.frames); await page.waitForTimeout(150);
        assert(await page.evaluate(count => HEGame.running && HEGame.metrics.frames > count, frame));
        // Engine stdout is split into lines. Count records, not exception names
        // repeated in the separately formatted stack/inner exception chain.
        const diagnosticLines = await page.evaluate(() => HEGame.entries);
        const faultLogs = diagnosticLines.filter(value => /\[Script\] instance=\d+, kind=\d+:/.test(value));
        assert.equal(faultLogs.length,1); checks.push('fault-isolation-no-log-loop');
        assert.ok(diagnosticLines.some(value => value.includes('at GameScripts.ScriptTouchSample.Update')));
        checks.push('managed-gameplay-stack');
        await page.getByRole('button',{name:'重新进入场景',exact:true}).click();
        await page.waitForFunction(() => HEGame.fields().throwOnUpdate.value === false);
        assert.equal((await fields()).score.value,3); checks.push('scene-reentry');
        await page.getByRole('button',{name:'销毁实体',exact:true}).click();
        await page.waitForFunction(() => HEGame.fields() === null);
        assert(await page.evaluate(() => HEGame.running)); checks.push('entity-destroy');
        await page.getByRole('button',{name:'重新进入场景',exact:true}).click();
        await page.waitForFunction(() => HEGame.fields()?.enableCount.value === 1);
        const measuredRun = await page.evaluate(() => ({buildId:HEGame.manifest.buildId, gameplaySourceIdentity:HEGame.manifest.gameplaySourceIdentity, managedIdentity:HEGame.manifest.managedIdentity,
            metrics:HEGame.metrics, bridge:HEGame.transport.metrics, memory:HEGame.memory()}));
        const legacy = await save(); delete legacy.sprite;
        await page.evaluate(state => localStorage.setItem('HEngine.ScriptSample.v1.scene',JSON.stringify(state)),legacy);
        await page.reload(); await ready();
        assert.equal((await fields()).score.value,3);
        assert.deepEqual((await save()).sprite.Color,persisted.sprite.Color); checks.push('legacy-save-without-sprite');
        const invalidContext = await browser.newContext();
        try {
            const invalid = structuredClone(persisted); invalid.sprite.Texture = '../outside.png';
            await invalidContext.addInitScript(state => localStorage.setItem('HEngine.ScriptSample.v1.scene',JSON.stringify(state)),invalid);
            const invalidPage = await invalidContext.newPage(); await invalidPage.goto(url);
            await invalidPage.waitForFunction(() => globalThis.HEGame?.error);
            assert.match(await invalidPage.evaluate(() => HEGame.error),/Saved sprite differs from the exported texture profile/);
            checks.push('invalid-saved-sprite-rejected');
        } finally { await invalidContext.close(); }
        const mismatchedContext = await browser.newContext();
        try {
            const mismatched = await mismatchedContext.newPage();
            await mismatched.route('**/game-manifest.json', async route => {
                const response = await route.fetch();
                const manifest = await response.json();
                // Preserve the DLL identity while replacing its registration generation.
                manifest.registration.gameplaySourceIdentity = '0'.repeat(64);
                await route.fulfill({response, json:manifest});
            });
            await mismatched.goto(url);
            await mismatched.waitForFunction(() => globalThis.HEGame?.error);
            assert.match(await mismatched.evaluate(() => HEGame.error), /Managed registry differs/);
            assert.equal(await mismatched.evaluate(() => !!HEGame.running), false);
            assert.equal(await mismatched.evaluate(() => HEGame.entries.some(value => value.includes('Initialize entity='))), false);
            checks.push('mismatched-registration-rejected-before-play');
        } finally { await mismatchedContext.close(); }
        assert.deepEqual(errors,[]);
        result = {...result,status:'PASS',checks,...measuredRun};
    } finally {
        fs.writeFileSync(path.join(evidence,'console.log'),logs.join('\n'));
        fs.writeFileSync(path.join(evidence,'errors.json'),JSON.stringify(errors,null,2));
        fs.writeFileSync(path.join(evidence,'result.json'),JSON.stringify(result,null,2));
        await browser.close();
    }
    console.log(JSON.stringify(result));
}
main().catch(error => {console.error(error.stack || error);process.exitCode=1;});
