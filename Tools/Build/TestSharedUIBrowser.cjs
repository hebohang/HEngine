'use strict';
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),{spawn}=require('node:child_process');
const {chromium}=require('playwright');
async function main(){
    const root=path.resolve(__dirname,'../..'),out=path.join(root,'out/agent/shared-ui-browser');fs.mkdirSync(out,{recursive:true});
    const server=spawn(process.execPath,[path.join(__dirname,'ServeWeb.cjs'),path.join(root,'Project/Dist/Web'),'0'],{windowsHide:true,stdio:['ignore','pipe','pipe']});let browser;
    try{
        const url=await new Promise((resolve,reject)=>{const timer=setTimeout(()=>reject(Error('Web server startup timeout')),15000);server.stdout.on('data',b=>{const m=String(b).match(/http:\/\/127\.0\.0\.1:\d+\//);if(m){clearTimeout(timer);resolve(m[0]);}});server.on('error',reject);server.on('exit',code=>{clearTimeout(timer);reject(Error('Web server exited '+code));});});
        browser=await chromium.launch({headless:true,executablePath:'C:/Program Files/Google/Chrome/Application/chrome.exe'});
        const page=await browser.newPage({viewport:{width:1280,height:720}}),errors=[];page.on('pageerror',e=>errors.push(String(e)));
        // Simulate an Editor source edit through the actual exported resource URL.
        const source=JSON.parse(fs.readFileSync(path.join(root,'Project/Assets/UI/SharedPause.hui')));
        source.Nodes.find(n=>n.ElementId==='shared_pause_title').Text='共享界面编辑验证';
        source.Nodes.find(n=>n.ElementId==='shared_resume').Style.BackgroundColor='#ffcc66';
        await page.route('**/Assets/UI/SharedPause.hui',route=>route.fulfill({json:source}));
        await page.goto(url);await page.waitForFunction(()=>window.HEGame?.running,null,{timeout:90000});
        await page.evaluate(()=>{HEGame.command('start');HEGame.command('pause');});
        const title=page.locator('[data-hui-element="shared_pause_title"]'),resume=page.locator('[data-hui-element="shared_resume"]');
        await title.waitFor({state:'visible'});assert.equal(await title.innerText(),'共享界面编辑验证');
        assert.equal(await resume.evaluate(e=>getComputedStyle(e).backgroundColor),'rgb(255, 204, 102)');
        for(const [w,h] of [[1280,720],[390,844],[844,390]]){
            await page.setViewportSize({width:w,height:h});
            await page.evaluate(()=>new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(resolve))));
            const box=await resume.boundingBox();assert.ok(box.x>=0&&box.y>=0&&box.x+box.width<=w&&box.y+box.height<=h,`button must remain in ${w}x${h}: ${JSON.stringify(box)}`);
        }
        await page.setViewportSize({width:1280,height:720});await page.screenshot({path:path.join(out,'shared-pause.png')});
        await resume.click();await page.waitForFunction(()=>HEGame.snapshot.mode==='playing');
        await page.evaluate(()=>HEGame.command('pause'));
        await page.locator('[data-hui-element="shared_restart"]').click();await page.waitForFunction(()=>HEGame.snapshot.mode==='playing'&&HEGame.snapshot.elapsed<2);
        await page.evaluate(()=>HEGame.command('pause'));
        await page.locator('[data-hui-element="shared_abandon"]').click();await page.waitForFunction(()=>HEGame.snapshot.mode==='result');
        assert.deepEqual(errors,[]);
        console.log('test-layer PASS: exported C# Web game, shared UI source text/color edits, landscape/portrait layout, pause/resume/restart/abandon clicks');
    }finally{if(browser)await browser.close();server.kill();}
}
main().catch(error=>{console.error(error);process.exitCode=1;});
