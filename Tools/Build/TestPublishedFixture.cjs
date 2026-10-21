'use strict';
const { spawn } = require('node:child_process'), path = require('node:path'), assert = require('node:assert/strict');
const { chromium } = require('playwright');
(async () => {
    const root = path.resolve(__dirname, '../../out/agent/publish-tests/Independent Project/Dist/Web');
    const server = spawn(process.execPath, [path.join(__dirname, 'ServeWeb.cjs'), root, '0'], { windowsHide: true, stdio: ['ignore','pipe','pipe'] });
    let browser;
    try {
        const url = await new Promise((resolve,reject) => {
            server.stdout.on('data', data => { const match = data.toString().match(/http:\/\/127\.0\.0\.1:\d+\//); if (match) resolve(match[0]); });
            server.on('error',reject); server.on('exit',code=>reject(Error('Server exit '+code)));
        });
        browser = await chromium.launch({ executablePath: process.env.HE_SCRIPT_CHROME || 'C:/Program Files/Google/Chrome/Application/chrome.exe', headless:true });
        const page = await browser.newPage(), errors = [];
        page.on('pageerror', error => errors.push(String(error)));
        await page.goto(url); await page.waitForFunction(() => globalThis.fixtureReady, null, { timeout:120000 });
        assert.equal(await page.locator('#value').innerText(), '42'); assert.deepEqual(errors,[]);
        const response = await page.request.get(url + 'engine/SceneRenderer.js'); assert.equal(response.status(),200);
        console.log('runtime-manual PASS: independent project C# Wasm returns 42; shared joystick and renderer modules available');
    } finally { if (browser) await browser.close(); server.kill(); }
})().catch(error=>{console.error(error);process.exitCode=1;});
