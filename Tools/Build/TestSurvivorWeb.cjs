'use strict';
const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict'),{spawn}=require('node:child_process');
const {chromium}=require('playwright');
async function main(){
    const backend=process.argv[2]||'auto';assert.ok(['auto','webgl2','webgpu'].includes(backend));
    const root=path.resolve(__dirname,'../..'),out=path.join(root,'out/agent/web-parity-test',backend);fs.mkdirSync(out,{recursive:true});
    const server=spawn(process.execPath,[path.join(__dirname,'ServeWeb.cjs'),path.join(root,'Project/Dist/Web'),'0'],{windowsHide:true,stdio:['ignore','pipe','pipe']});let browser;
    try{
        const url=await new Promise((resolve,reject)=>{server.stdout.on('data',b=>{const m=String(b).match(/http:\/\/127\.0\.0\.1:\d+\//);if(m)resolve(m[0]);});server.on('error',reject);});
        browser=await chromium.launch({headless:true,executablePath:'C:/Program Files/Google/Chrome/Application/chrome.exe'});
        const page=await browser.newPage({viewport:{width:1280,height:720},deviceScaleFactor:2}),errors=[];
        page.on('pageerror',e=>errors.push(String(e)));
        await page.goto(url+'?renderer='+backend);await page.waitForFunction(()=>globalThis.HEGame?.running,null,{timeout:90000});
        const activeBackend=await page.evaluate(()=>HEGame.renderer.diagnostics().backend);
        if(backend!=='auto')assert.equal(activeBackend,backend);
        assert.equal(await page.evaluate(()=>HEGame.snapshot.mode),'menu');
        const panel=await page.locator('.menu-panel').boundingBox();assert.ok(panel.width>1200);
        await page.screenshot({path:path.join(out,'menu.png'),scale:'css'});
        await page.locator('[data-tab="settings"]').click();await page.locator('#weaponSlots').selectOption('3');
        assert.equal(await page.evaluate(()=>HEGame.snapshot.profile.selectedWeaponSlotLimit),3);
        await page.locator('[data-action="start"]').click();await page.waitForFunction(()=>HEGame.snapshot.mode==='playing');
        assert.deepEqual(await page.evaluate(()=>HEGame.renderer.diagnostics().size),[2560,1440]);
        const before=await page.evaluate(()=>({x:HEGame.snapshot.x,z:HEGame.snapshot.z}));
        await page.keyboard.down('d');await page.waitForTimeout(350);await page.keyboard.up('d');
        assert.ok(await page.evaluate(p=>Math.hypot(HEGame.snapshot.x-p.x,HEGame.snapshot.z-p.z)>.3,before));
        await page.keyboard.press('Space');await page.waitForTimeout(60);assert.ok(await page.evaluate(()=>HEGame.snapshot.y>0));
        const yaw=await page.evaluate(()=>HEGame.snapshot.cameraYaw);await page.mouse.move(700,400);await page.mouse.down({button:'right'});await page.mouse.move(800,420);await page.mouse.up({button:'right'});
        assert.notEqual(await page.evaluate(()=>HEGame.snapshot.cameraYaw),yaw);
        await page.screenshot({path:path.join(out,'game.png'),scale:'css'});
        await page.keyboard.press('Escape');await page.waitForFunction(()=>HEGame.snapshot.mode==='paused');await page.locator('[data-hui-element="shared_resume"]').click();
        await page.evaluate(()=>window.dispatchEvent(new Event('blur')));assert.equal(await page.evaluate(()=>HEGame.snapshot.mode),'paused');
        await page.locator('[data-hui-element="shared_resume"]').click();await page.evaluate(()=>HEGame.advance(30));
        await page.waitForFunction(()=>HEGame.snapshot.mode==='upgrade');
        const cards=page.locator('[data-action="upgrade"]');assert.equal(await cards.count(),3);
        await page.screenshot({path:path.join(out,'upgrade.png'),scale:'css'});
        await cards.first().locator('img').click();assert.notEqual(await page.evaluate(()=>HEGame.snapshot.mode),'upgrade');
        const mobile=await browser.newPage({viewport:{width:890,height:400},deviceScaleFactor:3,isMobile:true,hasTouch:true});mobile.on('pageerror',e=>errors.push(String(e)));
        await mobile.addInitScript(()=>{const original=WebGL2RenderingContext.prototype.getParameter;WebGL2RenderingContext.prototype.getParameter=function(p){return p===this.MAX_VERTEX_UNIFORM_VECTORS?256:original.call(this,p);};});
        await mobile.goto(url+'?renderer='+backend);await mobile.waitForFunction(()=>globalThis.HEGame?.running,null,{timeout:90000});
        await mobile.screenshot({path:path.join(out,'mobile-menu.png'),scale:'css'});
        await mobile.locator('[data-action="start"]').tap();await mobile.waitForFunction(()=>HEGame.snapshot.mode==='playing');
        await mobile.locator('#jump').tap();await mobile.waitForTimeout(60);assert.ok(await mobile.evaluate(()=>HEGame.snapshot.y>0));
        assert.equal(await mobile.locator('#joystick').isVisible(),true);
        const mobileBefore=await mobile.evaluate(()=>({x:HEGame.snapshot.x,z:HEGame.snapshot.z,yaw:HEGame.snapshot.cameraYaw}));
        const cdp=await mobile.context().newCDPSession(mobile);
        await cdp.send('Input.dispatchTouchEvent',{type:'touchStart',touchPoints:[{x:142,y:312,id:1},{x:650,y:240,id:2}]});
        await cdp.send('Input.dispatchTouchEvent',{type:'touchMove',touchPoints:[{x:182,y:312,id:1},{x:710,y:240,id:2}]});
        await mobile.waitForTimeout(350);await cdp.send('Input.dispatchTouchEvent',{type:'touchEnd',touchPoints:[]});
        assert.ok(await mobile.evaluate(p=>Math.hypot(HEGame.snapshot.x-p.x,HEGame.snapshot.z-p.z)>.3,mobileBefore));
        assert.notEqual(await mobile.evaluate(()=>HEGame.snapshot.cameraYaw),mobileBefore.yaw);
        await mobile.screenshot({path:path.join(out,'mobile-game.png'),scale:'css'});
        await mobile.setViewportSize({width:390,height:844});await mobile.waitForTimeout(200);await mobile.screenshot({path:path.join(out,'portrait-game.png'),scale:'css'});
        if(activeBackend==='webgpu'){
            await page.evaluate(()=>{if(HEGame.snapshot.mode==='playing')HEGame.command('pause');});
            const beforeLoss=await page.evaluate(()=>({save:HEGame.save(),mode:HEGame.snapshot.mode}));
            await page.evaluate(()=>HEGame.renderer.graphics.device.destroy());
            await page.waitForFunction(()=>HEGame.renderer.diagnostics().backend==='webgl2',null,{timeout:90000});
            assert.equal(await page.evaluate(()=>HEGame.running),true);
            assert.equal(await page.evaluate(()=>HEGame.save()),beforeLoss.save);
            assert.equal(await page.evaluate(()=>HEGame.snapshot.mode),beforeLoss.mode==='playing'?'paused':beforeLoss.mode);
            await page.screenshot({path:path.join(out,'device-recovered.png'),scale:'css'});
        }
        assert.deepEqual(errors,[]);console.log('runtime-manual-browser PASS ('+backend+' -> '+activeBackend+'): Web C# runtime, menu, DPR 2/3, settings, movement/jump/look/pause/blur, upgrade, touch and resize'+(activeBackend==='webgpu'?'; device-loss recovery preserves game/save state':''));
    }finally{if(browser)await browser.close();server.kill();}
}
main().catch(error=>{console.error(error);process.exitCode=1;});
