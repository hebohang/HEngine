'use strict';
const fs = require('node:fs');
const path = require('node:path');
const http = require('node:http');
const assert = require('node:assert/strict');
const {chromium} = require('playwright');
const root = path.resolve(__dirname, '../..');
const work = path.join(root, 'out/agent/survivor-parity');
const site = path.join(work, 'web-publish/wwwroot');

async function main() {
    for (const name of ['index.html', 'main.js'])
        fs.copyFileSync(path.join(__dirname, 'SurvivorParityWeb', name), path.join(site, name));
    fs.copyFileSync(path.join(work, 'python-reference.json'), path.join(site, 'python-reference.json'));
    const types = {'.html':'text/html; charset=utf-8', '.js':'text/javascript', '.wasm':'application/wasm', '.json':'application/json'};
    const server = http.createServer((req, res) => {
        try {
            const file = path.resolve(site, '.' + new URL(req.url, 'http://localhost').pathname.replace(/\/$/, '/index.html'));
            if (!file.startsWith(site + path.sep)) { res.writeHead(403).end(); return; }
            const bytes = fs.readFileSync(file);
            res.writeHead(200, {'Content-Type':types[path.extname(file)] || 'application/octet-stream'}).end(bytes);
        } catch { res.writeHead(404).end(); }
    });
    let browser;
    const errors = [];
    try {
        await new Promise((resolve, reject) => { server.once('error', reject); server.listen(0, '127.0.0.1', resolve); });
        browser = await chromium.launch({headless:true, executablePath:process.env.HE_SCRIPT_CHROME || 'C:/Program Files/Google/Chrome/Application/chrome.exe'});
        const page = await browser.newPage();
        page.on('pageerror', error => errors.push(String(error)));
        await page.goto(`http://127.0.0.1:${server.address().port}/`);
        await page.waitForFunction(() => globalThis.survivorParity, null, {timeout:120000});
        const result = await page.evaluate(() => globalThis.survivorParity);
        fs.writeFileSync(path.join(work, 'web-result.json'), JSON.stringify({evidenceType:'unit-test-browser-wasm', result, errors}, null, 2));
        assert.equal(result.passed, true, result.error);
        assert.deepEqual(errors, []);
        console.log('browser-wasm ' + result.result);
    } finally {
        if (browser) await browser.close();
        await new Promise(resolve => server.close(resolve));
    }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
