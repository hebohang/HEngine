'use strict';
// Headed real-game measurement; the wrapper only times and forwards engine calls.
const fs = require('node:fs'), path = require('node:path'), assert = require('node:assert/strict');
const {chromium} = require('playwright');
const {spawn} = require('node:child_process');
const output = path.resolve(process.argv[2] || 'out/agent/scripting/web-performance');
fs.mkdirSync(output, {recursive:true});
const port = Number(process.argv[3] || 8772), traceRequested = process.argv.includes('--trace');
const packageDirectory = path.resolve(__dirname,'../../out/agent/scripting/game-web');
const expected = JSON.parse(fs.readFileSync(path.join(packageDirectory,'game-manifest.json'),'utf8'));
async function startServer() {
    const child = spawn(process.execPath,[path.join(__dirname,'serve-web.cjs'),packageDirectory,String(port)],{windowsHide:true,stdio:['ignore','pipe','pipe']});
    let logs = '';
    const record = data => { logs = (logs + data.toString()).slice(-65536); };
    child.stdout.on('data',record); child.stderr.on('data',record);
    try {
        await new Promise((resolve,reject) => {
            const timer = setTimeout(() => reject(new Error('Owned HTTP server readiness timed out.')),10000);
            const ready = data => { if (data.toString().includes(`http://127.0.0.1:${port}/`)) { clearTimeout(timer); resolve(); } };
            child.stdout.on('data',ready);
            child.once('error',error => { clearTimeout(timer); reject(error); });
            child.once('exit',code => { clearTimeout(timer); reject(new Error('Owned HTTP server exited: '+code)); });
        });
        return {child,logs:() => logs};
    } catch(error) { await stopServer(child); fs.writeFileSync(path.join(output,'server.log'),logs); throw error; }
}
async function stopServer(child) {
    if (!child.pid || child.exitCode !== null || child.signalCode !== null) return;
    await new Promise((resolve,reject) => {
        const timer = setTimeout(() => reject(new Error('Owned HTTP server did not exit.')),5000);
        child.once('exit',() => {clearTimeout(timer);resolve();}); child.kill();
    });
}
async function saveTrace(session) {
    let timer;
    const complete = new Promise((resolve,reject) => {
        timer = setTimeout(() => reject(new Error('Browser trace completion timed out.')),30000);
        session.once('Tracing.tracingComplete',resolve);
    });
    let receipt;
    try { await session.send('Tracing.end'); receipt = await complete; } finally {clearTimeout(timer);}
    assert(receipt.stream,'Browser did not return a trace stream.');
    const file = path.join(output,'browser-trace.json'), fd = fs.openSync(file,'w');
    let bytes = 0;
    try {
        for (;;) {
            const chunk = await session.send('IO.read',{handle:receipt.stream,size:65536});
            const data = Buffer.from(chunk.data,chunk.base64Encoded ? 'base64' : 'utf8');
            bytes += data.length;
            if (bytes > 256*1024*1024) throw new Error('Browser trace exceeded 256 MiB.');
            fs.writeSync(fd,data);
            if (chunk.eof) break;
        }
    } finally {fs.closeSync(fd); await session.send('IO.close',{handle:receipt.stream});}
    assert(!receipt.dataLossOccurred,'Browser reported lost trace events.');
    return {file,bytes,dataLossOccurred:receipt.dataLossOccurred ?? false};
}
function summarize(values) {
    const sorted = [...values].sort((a,b) => a-b);
    return {count:values.length, mean:values.reduce((a,b) => a+b,0)/values.length,
        p95:sorted[Math.ceil(sorted.length*.95)-1], p99:sorted[Math.ceil(sorted.length*.99)-1], max:sorted.at(-1)};
}
(async () => {
    let browser, server, tracing, traceActive = false;
    const result = {status:'FAIL', measurement:'Desktop headed Chromium, real engine+Mono. Engine call CPU includes native script synchronization/render submission, excludes GPU completion and host status UI. Disabled-script phase is an internal comparison, not a C++ gameplay baseline. Browser-reported JS heap is an estimate, not total RSS; do not sum it with linear memory as if their accounting were disjoint.'};
    const errors = [];
    try {
        server = await startServer();
        browser = await chromium.launch({headless:false, executablePath:process.env.HE_SCRIPT_CHROME || 'C:/Program Files/Google/Chrome/Application/chrome.exe'});
        result.browserVersion = browser.version();
        result.startupScope = 'New browser profile/context; OS file and GPU caches uncontrolled. Timer starts in the game module, excluding earlier browser navigation/import time.';
        const page = await browser.newPage({viewport:{width:1200,height:1000}});
        page.on('pageerror', error => errors.push(String(error)));
        if (traceRequested) {
            tracing = await page.context().newCDPSession(page);
            await tracing.send('Tracing.start',{transferMode:'ReturnAsStream',traceConfig:{recordMode:'recordUntilFull',traceBufferSizeInKb:65536,
                includedCategories:['devtools.timeline','blink.user_timing','v8','disabled-by-default-v8.gc']}});
            traceActive = true;
        }
        await page.goto(`http://127.0.0.1:${port}/`);
        await page.waitForFunction(() => globalThis.HEGame?.running && HEGame.metrics.frames > 120, null, {timeout:30000});
        result.identity = await page.evaluate(() => ({buildId:HEGame.manifest.buildId, managedIdentity:HEGame.manifest.managedIdentity, firstGameFrameMs:HEGame.metrics.firstGameFrameMs}));
        assert.equal(result.identity.buildId,expected.buildId,'The running game differs from the selected package.');
        assert.equal(result.identity.managedIdentity,expected.managedIdentity);
        await page.evaluate(trace => {
            const original = HEGame.engine._he_frame;
            globalThis.hePerformance = {samples:[], previous:null, gcTimes:[]};
            HEGame.engine._he_frame = function(...args) {
                const before = performance.now(), value = original.apply(this,args), after = performance.now();
                const state = globalThis.hePerformance;
                if (state.previous !== null) {
                    state.samples.push({start:before, cpu:after-before, interval:before-state.previous});
                    if (trace && before-state.previous > 33) performance.measure('HEngine.LongInterval',{start:state.previous,end:before});
                }
                if (trace && after-before > 20) performance.measure('HEngine.LongEngineCall',{start:before,end:after});
                state.previous = before;
                return value;
            };
        },traceRequested);
        const sample = async (name, collect = false) => {
            const before = await page.evaluate(phase => { performance.mark('HEngine.Phase.'+phase); hePerformance.samples=[]; hePerformance.previous=null; return structuredClone(HEGame.transport.metrics); },name);
            if (collect) {
                for (let i=0;i<20;i++) {
                    await page.waitForTimeout(150);
                    await page.evaluate(() => { const start=performance.now(); hePerformance.gcTimes.push(start); document.getElementById('collect').click(); performance.measure('HEngine.ForcedManagedGC',{start,end:performance.now()}); });
                }
            }
            await page.waitForFunction(() => hePerformance.samples.length >= 600, null, {timeout:30000});
            const captured = await page.evaluate(() => ({samples:hePerformance.samples, bridge:HEGame.transport.metrics, gc:HEGame.metrics.gc, gcTimes:hePerformance.gcTimes,
                memory:HEGame.memory(), browserHeap:performance.memory ? {usedJSHeapSize:performance.memory.usedJSHeapSize,totalJSHeapSize:performance.memory.totalJSHeapSize,jsHeapSizeLimit:performance.memory.jsHeapSizeLimit}:null}));
            const calls = captured.bridge.calls-before.calls, milliseconds = captured.bridge.milliseconds-before.milliseconds;
            result[name] = {engineCallCpuMs:summarize(captured.samples.map(s=>s.cpu)), frameIntervalMs:summarize(captured.samples.map(s=>s.interval)),
                bridge:{calls,milliseconds,meanMs:calls ? milliseconds/calls : null}, memory:captured.memory,browserHeap:captured.browserHeap};
            if (collect) {
                result[name].collections = captured.gc;
                result[name].firstFrameAfterGc = captured.gcTimes.map(t => captured.samples.find(s => s.start>=t)).filter(Boolean);
            }
            fs.writeFileSync(path.join(output,name+'-frames.json'),JSON.stringify(captured.samples));
        };
        await sample('enabled');
        await page.getByRole('button',{name:'禁用脚本',exact:true}).click();
        await sample('disabled');
        await page.getByRole('button',{name:'启用脚本',exact:true}).click();
        await sample('forcedGc',true);
        assert.equal(errors.length,0);
        assert(await page.evaluate(() => HEGame.running));
        if (tracing) { result.trace = await saveTrace(tracing); traceActive = false; }
        assert.equal(JSON.parse(fs.readFileSync(path.join(packageDirectory,'game-manifest.json'),'utf8')).buildId,expected.buildId,'Package changed during measurement.');
        result.status='PASS';
    } catch (error) { result.error=String(error.stack || error); throw error; }
    finally {
        try {
            if (traceActive) { try { result.trace = await saveTrace(tracing); } catch(error) { result.traceError=String(error); } }
            if (browser) await browser.close();
        } catch(error) { result.status='FAIL'; result.cleanupError=String(error); throw error; } finally {
            try { if (server) { await stopServer(server.child); fs.writeFileSync(path.join(output,'server.log'),server.logs()); } }
            catch(error) { result.status='FAIL'; result.cleanupError=String(error); throw error; }
            finally { fs.writeFileSync(path.join(output,'result.json'),JSON.stringify({...result,errors},null,2)); }
        }
    }
})().catch(error => { console.error(error); process.exitCode=1; });
