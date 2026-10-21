'use strict';
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),{spawn}=require('node:child_process');
const {chromium}=require('playwright');
async function main(){
    const project=path.resolve(process.argv[2]),out=path.resolve(process.argv[3]||'out/agent/shared-arena-web');
    fs.mkdirSync(out,{recursive:true});
    const server=spawn(process.execPath,[path.join(__dirname,'ServeWeb.cjs'),path.join(project,'Dist/Web'),'0'],{windowsHide:true,stdio:['ignore','pipe','pipe']});
    let browser;
    try {
        const url=await new Promise((resolve,reject)=>{
            const timer=setTimeout(()=>reject(Error('Owned HTTP server timed out')),10000);
            server.stdout.on('data',b=>{const m=String(b).match(/http:\/\/127\.0\.0\.1:\d+\//);if(m){clearTimeout(timer);resolve(m[0]);}});
            server.once('error',e=>{clearTimeout(timer);reject(e);});server.once('exit',()=>{clearTimeout(timer);reject(Error('Owned HTTP server exited'));});
        });
        browser=await chromium.launch({headless:true,executablePath:'C:/Program Files/Google/Chrome/Application/chrome.exe'});
        const page=await browser.newPage({viewport:{width:1280,height:720}}),errors=[];
        page.on('pageerror',e=>{errors.push(String(e));console.error('Browser error: '+String(e));});
        await page.goto(url);await page.waitForFunction(()=>globalThis.HEGame?.running,null,{timeout:60000});
        const action=async()=>{await page.waitForFunction(()=>HEGame.buttons.length>0);const b=await page.evaluate(()=>HEGame.buttons[0]);await page.mouse.click(b.x+b.width/2,b.y+b.height/2);};
        assert.equal(await page.evaluate(()=>HEGame.snapshot.mode),'menu');await action();
        await page.waitForFunction(()=>HEGame.snapshot.mode==='playing');
        const player=()=>page.evaluate(()=>HEGame.snapshot.objects.find(o=>o.id==='player'));
        const before=await player();await page.keyboard.down('d');await page.waitForTimeout(350);await page.keyboard.up('d');
        assert.ok((await player()).x>before.x+.5,'Player movement');
        await page.screenshot({path:path.join(out,'game.png')});
        await action();await page.waitForFunction(()=>HEGame.snapshot.mode==='paused');
        const paused=await player();await page.keyboard.down('d');await page.waitForTimeout(200);await page.keyboard.up('d');assert.deepEqual(await player(),paused);
        await action();await page.waitForFunction(()=>HEGame.snapshot.mode==='playing');
        await page.evaluate(()=>window.dispatchEvent(new Event('blur')));assert.equal(await page.evaluate(()=>HEGame.snapshot.mode),'paused');
        await page.reload();await page.waitForFunction(()=>globalThis.HEGame?.running);assert.equal(await page.evaluate(()=>HEGame.snapshot.mode),'menu');
        assert.equal(JSON.parse(await page.evaluate(()=>HEGame.save())).schema,1);
        await page.setViewportSize({width:890,height:400});await page.waitForTimeout(100);await action();
        const touchBefore=await player();
        const touch=await page.context().newCDPSession(page);
        await touch.send('Input.dispatchTouchEvent',{type:'touchStart',touchPoints:[{id:7,x:100,y:220}]});
        await touch.send('Input.dispatchTouchEvent',{type:'touchMove',touchPoints:[{id:7,x:160,y:220}]});
        await page.waitForTimeout(300);
        await touch.send('Input.dispatchTouchEvent',{type:'touchEnd',touchPoints:[]});
        assert.ok((await player()).x>touchBefore.x+.5,'Touch movement');
        await page.screenshot({path:path.join(out,'touch.png')});
        assert.deepEqual(errors,[]);
        fs.writeFileSync(path.join(out,'result.json'),JSON.stringify({schema:1,pass:true,evidence:'test-layer',project,checks:['Wasm startup','menu/start','movement','pause/resume','blur','save reload','touch/resize','no page errors']},null,2));
        console.log('SharedArena Web interaction PASS: '+out);
    } finally { if(browser)await browser.close();server.kill(); }
}
main().catch(error=>{console.error(error);process.exitCode=1;});
