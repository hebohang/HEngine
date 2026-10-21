'use strict';
const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict'),{spawn}=require('node:child_process');
const {chromium}=require('playwright');
const root=path.resolve(__dirname,'../..'),work=path.join(root,'out/agent/survivor-web/evidence');
fs.mkdirSync(work,{recursive:true});
async function main(){
    let browser;const errors=[],checks=[];
    const server=process.env.HE_WEB_BASE_URL?null:spawn(process.execPath,[path.join(__dirname,'ServeSurvivorWeb.cjs'),'0'],{windowsHide:true,stdio:['ignore','pipe','pipe']});
    try{
        const url=process.env.HE_WEB_BASE_URL||await new Promise((resolve,reject)=>{let text='';server.stdout.on('data',b=>{text+=b;const m=text.match(/http:\/\/127\.0\.0\.1:\d+\//);if(m)resolve(m[0]);});server.on('error',reject);server.on('exit',code=>reject(Error('Server exited '+code)));setTimeout(()=>reject(Error('Server timeout')),10000).unref();});
        browser=await chromium.launch({headless:true,executablePath:process.env.HE_SCRIPT_CHROME||'C:/Program Files/Google/Chrome/Application/chrome.exe'});
        const page=await browser.newPage({viewport:{width:1280,height:800}});
        page.on('pageerror',e=>errors.push(String(e)));page.on('console',m=>{if(m.type()==='error')errors.push(m.text());});
        await page.goto(url);await page.waitForFunction(()=>globalThis.HEGame?.running||globalThis.HEGame?.error,null,{timeout:120000});
        assert.equal(await page.evaluate(()=>HEGame.snapshot.mode),'menu');checks.push('original-assets-menu');
        await page.screenshot({path:path.join(work,'menu.png')});
        await page.locator('[data-action="start"]').click();await page.waitForTimeout(1500);
        assert.equal(await page.evaluate(()=>HEGame.snapshot.mode),'playing');
        const before=await page.evaluate(()=>HEGame.snapshot.x);await page.keyboard.down('d');await page.waitForTimeout(1300);await page.keyboard.up('d');
        assert.ok(await page.evaluate(()=>HEGame.snapshot.x)>before+1);checks.push('keyboard-movement');
        await page.mouse.move(640,400);await page.mouse.down({button:'right'});await page.mouse.move(700,420,{steps:6});await page.mouse.up({button:'right'});
        assert.ok(Math.abs(await page.evaluate(()=>HEGame.snapshot.cameraYaw))>.05);checks.push('right-drag-orbit-camera');
        await page.evaluate(()=>HEGame.command('look',JSON.stringify([HEGame.snapshot.cameraYaw/.006,(Math.atan2(14,17)-HEGame.snapshot.cameraElevation)/.004])));
        await page.screenshot({path:path.join(work,'game.png')});
        await page.keyboard.press('Escape');const paused=await page.evaluate(()=>HEGame.snapshot.elapsed);await page.waitForTimeout(500);
        assert.equal(await page.evaluate(()=>HEGame.snapshot.elapsed),paused);checks.push('pause-freezes-simulation');
        await page.locator('[data-hui-element="shared_resume"]').click();
        let upgraded=false;
        for(let i=0;i<60;i++){
            const state=await page.evaluate(()=>HEGame.advance(1));
            if(state.mode==='upgrade'){await page.locator('[data-action="upgrade"]').first().click();upgraded=true;break;}
            if(state.mode==='result')break;
        }
        assert.ok(upgraded,'No experience upgrade reached');checks.push('combat-kill-pickup-upgrade');
        await page.evaluate(()=>HEGame.command('pause'));await page.locator('[data-hui-element="shared_abandon"]').click();
        assert.equal(await page.evaluate(()=>HEGame.snapshot.mode),'result');
        const gold=await page.evaluate(()=>HEGame.snapshot.profile.walletGold),runs=await page.evaluate(()=>HEGame.snapshot.profile.career.runs);
        assert.ok(gold>0,'A level-2 abandoned run must settle nonzero level gold');
        assert.equal(await page.evaluate(()=>HEGame.snapshot.profile.lastRun.rewardCommitted),true);
        assert.equal(await page.evaluate(()=>HEGame.snapshot.profile.lastRun.reward.earnedGold),gold);
        await page.screenshot({path:path.join(work,'result.png')});
        await page.locator('[data-action="home"]').click();await page.reload();await page.waitForFunction(()=>globalThis.HEGame?.running,null,{timeout:120000});
        assert.equal(await page.evaluate(()=>HEGame.snapshot.profile.walletGold),gold);assert.equal(await page.evaluate(()=>HEGame.snapshot.profile.career.runs),runs);checks.push('settlement-home-refresh-save');
        const firstRunId=await page.evaluate(()=>HEGame.snapshot.profile.lastRun.runId);
        await page.locator('[data-action="start"]').click();
        assert.equal(await page.evaluate(()=>HEGame.snapshot.mode),'playing');
        assert.notEqual(await page.evaluate(()=>HEGame.snapshot.profile.lastRun.runId),firstRunId);
        for(let i=0;i<60;i++){const s=await page.evaluate(()=>HEGame.advance(1));if(s.mode==='upgrade'||s.mode==='result')break;}
        await page.evaluate(()=>HEGame.command('abandon'));
        assert.ok(await page.evaluate(()=>HEGame.snapshot.profile.walletGold)>gold,'Second run did not credit reward');
        checks.push('second-run-independent-settlement');
        const recovery=await browser.newContext();
        const oldSave=JSON.parse(await page.evaluate(()=>HEGame.save()));delete oldSave.lastRun.runId;
        await recovery.addInitScript(save=>localStorage.setItem('HEngine.StarbloomSurvivor.CSharp.v4',save),JSON.stringify(oldSave));
        const recoveryPage=await recovery.newPage();await recoveryPage.goto(url);await recoveryPage.waitForFunction(()=>globalThis.HEGame?.running,null,{timeout:120000});
        assert.equal(await recoveryPage.evaluate(()=>HEGame.snapshot.profile.walletGold),oldSave.walletGold);
        assert.match(await recoveryPage.evaluate(()=>HEGame.snapshot.profile.lastRun.runId),/^recovered-/);
        checks.push('incomplete-preview-receipt-recovery');await recovery.close();
        // Isolated endgame fixture: shorten objective timers and boss health.
        // This exercises the branch, not a claim that a full normal run was played.
        const endContext=await browser.newContext({viewport:{width:1280,height:800}});
        await endContext.route('**/catalog.json',async route=>{
            const data=await (await route.fetch()).json();
            for(const region of Object.values(data.regions))region.authoredPosition=[0,0,0];
            data.objective.armingSeconds=.1;data.objective.assaultSeconds=.1;data.objective.centralHoldSeconds=.1;data.objective.bossIntroSeconds=.1;
            for(const enemy of [...data.enemies,...data.bosses])enemy.damage=0;
            const boss=data.bosses.find(b=>b.id==='star_dragon');boss.health=1;boss.speed=8;
            await route.fulfill({contentType:'application/json',body:JSON.stringify(data)});
        });
        const endPage=await endContext.newPage();endPage.on('pageerror',e=>errors.push(String(e)));
        await endPage.goto(url);await endPage.waitForFunction(()=>globalThis.HEGame?.running,null,{timeout:120000});
        await endPage.locator('[data-action="start"]').click();let victory=false;
        for(let i=0;i<90;i++){
            const state=await endPage.evaluate(()=>HEGame.advance(1));
            if(state.mode==='upgrade')await endPage.locator('[data-action="upgrade"]').first().click();
            if(state.mode==='result'){victory=state.profile.lastRun.endReason==='victory';break;}
        }
        assert.ok(victory,'Objective boss victory fixture failed');checks.push('fixture-four-regions-boss-victory');
        await endPage.screenshot({path:path.join(work,'victory-fixture.png')});await endContext.close();
        for(const weapon of ['ring','sporecap','frostorb','rainbowrail']){
            const ctx=await browser.newContext();
            await ctx.route('**/catalog.json',async route=>{
                const data=await (await route.fetch()).json();
                data.characters.find(c=>c.id==='starpaw').starterWeapon=weapon;
                for(const enemy of [...data.enemies,...data.bosses]){enemy.damage=0;enemy.health=1;}
                await route.fulfill({contentType:'application/json',body:JSON.stringify(data)});
            });
            const fixturePage=await ctx.newPage();fixturePage.on('pageerror',e=>errors.push(String(e)));
            await fixturePage.goto(url);await fixturePage.waitForFunction(()=>globalThis.HEGame?.running,null,{timeout:120000});
            await fixturePage.locator('[data-action="start"]').click();let kills=0;
            for(let i=0;i<40;i++){const s=await fixturePage.evaluate(()=>HEGame.advance(1));kills=s.kills;if(kills>0)break;}
            assert.ok(kills>0,weapon+' failed to damage enemies in isolated fixture');await ctx.close();
        }
        checks.push('fixture-ring-zone-orbit-beam-damage');
        const mobile=await browser.newContext({viewport:{width:390,height:844},hasTouch:true,isMobile:true});
        const mobilePage=await mobile.newPage();await mobilePage.goto(url);await mobilePage.waitForFunction(()=>globalThis.HEGame?.running,null,{timeout:120000});
        await mobilePage.locator('[data-action="start"]').click();const joystick=await mobilePage.locator('#joystick').boundingBox();assert.ok(joystick);
        const mx=joystick.x+joystick.width/2,my=joystick.y+joystick.height/2;
        await mobilePage.mouse.move(mx,my);await mobilePage.mouse.down();await mobilePage.mouse.move(mx+30,my);await mobilePage.waitForTimeout(1200);await mobilePage.mouse.up();
        assert.ok(await mobilePage.evaluate(()=>HEGame.snapshot.x)>1);checks.push('mobile-viewport-pointer-joystick');
        const cameraBefore=await mobilePage.evaluate(()=>HEGame.snapshot.cameraYaw);
        const touch=await mobile.newCDPSession(mobilePage);
        await touch.send('Input.dispatchTouchEvent',{type:'touchStart',touchPoints:[{x:280,y:400}]});
        await touch.send('Input.dispatchTouchEvent',{type:'touchMove',touchPoints:[{x:340,y:420}]});
        await touch.send('Input.dispatchTouchEvent',{type:'touchEnd',touchPoints:[]});
        await mobilePage.waitForTimeout(100);
        assert.ok(Math.abs(await mobilePage.evaluate(()=>HEGame.snapshot.cameraYaw)-cameraBefore)>.1);checks.push('mobile-touch-orbit-camera');
        await mobilePage.screenshot({path:path.join(work,'mobile.png')});await mobile.close();
        assert.deepEqual(errors,[]);assert.ok(await page.evaluate(()=>HEGame.renderer.drawCalls)>0);
        fs.writeFileSync(path.join(work,'result.json'),JSON.stringify({status:'PASS',evidenceType:'browser-integration',checks,errors,visualReview:'pending'},null,2));
        console.log('browser-integration PASS: '+checks.join(', '));
    }catch(error){fs.writeFileSync(path.join(work,'result.json'),JSON.stringify({status:'FAIL',checks,errors,error:String(error)},null,2));throw error;}
    finally{if(browser)await browser.close();server?.kill();}
}
main().catch(error=>{console.error(error);process.exitCode=1;});
