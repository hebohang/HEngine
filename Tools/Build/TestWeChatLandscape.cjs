'use strict';
// Run the actual exported game and C# Wasm with a strict package-path WX host.
const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict'),{spawn}=require('node:child_process');
const esbuild=require('../Scripting/node_modules/esbuild'),{chromium}=require('playwright');
async function main(){
    const root=path.resolve(__dirname,'../..'),pkg=path.resolve(process.argv[2]||path.join(root,'Project/Dist/WeChat')),out=path.join(root,'out/agent/wechat-landscape-test');fs.mkdirSync(out,{recursive:true});
    const bundled=esbuild.buildSync({entryPoints:[path.join(pkg,'game.js')],bundle:true,platform:'browser',write:false}).outputFiles[0].text;
    const server=spawn(process.execPath,[path.join(__dirname,'ServeWeb.cjs'),pkg,'0'],{windowsHide:true,stdio:['ignore','pipe','pipe']});let browser;
    try{
        const url=await new Promise((resolve,reject)=>{server.stdout.on('data',b=>{const m=String(b).match(/http:\/\/127\.0\.0\.1:\d+\//);if(m)resolve(m[0]);});server.on('error',reject);});
        browser=await chromium.launch({headless:true,executablePath:'C:/Program Files/Google/Chrome/Application/chrome.exe'});
        const page=await browser.newPage({viewport:{width:960,height:540}}),errors=[];page.on('pageerror',e=>errors.push(String(e)));
        await page.route('**/__test',r=>r.fulfill({contentType:'text/html',body:'<style>body{margin:0}canvas{display:block;width:100vw;height:100vh}</style>'}));await page.goto(url+'__test');
        await page.evaluate(()=>{
            window.GameGlobal=window;const buffers=new Map(),events={},storage=new Map();let mainCanvas;
            window.fixture={events,modals:[],toasts:[],size:{width:960,height:540}};
            const bytes=async file=>{if(!buffers.has(file)){const response=await fetch(file);if(!response.ok)throw Error('Missing '+file);buffers.set(file,await response.arrayBuffer());}return buffers.get(file);};
            const filePath=file=>{if(typeof file!=='string'||!file.endsWith('.wasm'))throw Error('WX Wasm requires a package path');};
            window.WXWebAssembly={Memory:WebAssembly.Memory,Table:WebAssembly.Table,Global:WebAssembly.Global,
                Module:function(file){filePath(file);return new WebAssembly.Module(buffers.get(file));},
                instantiate:async(file,imports)=>{filePath(file);return WebAssembly.instantiate(await bytes(file),imports);}};
            const shared=document.createElement('canvas');shared.width=720;shared.height=640;
            const originalDrawImage=CanvasRenderingContext2D.prototype.drawImage;
            CanvasRenderingContext2D.prototype.drawImage=function(source,...args){if(source===shared)throw Error('Restricted shared canvas cannot enter a 2D HUD');return originalDrawImage.call(this,source,...args);};
            window.wx={getWindowInfo:()=>({windowWidth:fixture.size.width,windowHeight:fixture.size.height,pixelRatio:3,safeArea:{left:12,top:0,right:fixture.size.width-12,bottom:fixture.size.height-10}}),
                getMenuButtonBoundingClientRect:()=>({bottom:40}),
                setEnableDebug:({enableDebug})=>{fixture.debug=enableDebug;},
                setDeviceOrientation:({value,complete})=>{fixture.orientation=value;complete();},
                createCanvas:()=>{const c=document.createElement('canvas');if(!mainCanvas){mainCanvas=c;document.body.appendChild(c);}return c;},createImage:()=>new Image(),
                getFileSystemManager:()=>({readFile:({filePath,success,fail})=>bytes(filePath).then(data=>success({data})).catch(fail),readFileSync:file=>new TextDecoder().decode(buffers.get(file))}),
                loadSubpackage:({success})=>success(),getStorageSync:key=>storage.get(key),setStorageSync:(key,value)=>storage.set(key,value),
                createInnerAudioContext:()=>({onError(){},play(){},pause(){},stop(){},destroy(){}}),
                showModal:options=>fixture.modals.push(options),showToast:options=>fixture.toasts.push(options),
                authorize:({success})=>success(),getOpenDataContext:()=>({canvas:shared,postMessage:()=>{const ctx=shared.getContext('2d');ctx.fillStyle='#234';ctx.fillRect(0,0,720,640);ctx.fillStyle='white';ctx.fillText('Fixture friend rankings',20,40);}})};
            for(const name of ['TouchStart','TouchMove','TouchEnd','TouchCancel','Hide','Show','WindowResize'])wx['on'+name]=fn=>events[name]=fn;
            fixture.tap=(x,y)=>{events.TouchStart({changedTouches:[{identifier:1,clientX:x,clientY:y}]});events.TouchEnd({changedTouches:[{identifier:1,clientX:x,clientY:y}]});};
        });
        await page.addScriptTag({content:bundled});await page.waitForFunction(()=>window.HEGame||fixture.modals.length,null,{timeout:90000});
        assert.deepEqual(await page.evaluate(()=>fixture.modals),[]);assert.equal(await page.evaluate(()=>HEGame.snapshot.mode),'menu');
        assert.equal(await page.evaluate(()=>fixture.orientation),'landscape');
        assert.equal(await page.evaluate(()=>fixture.debug),false);
        assert.deepEqual(await page.evaluate(()=>HEGame.renderer.diagnostics().size),[2880,1620]);
        assert.deepEqual(await page.evaluate(()=>({width:HEGame.ui.rasterWidth,height:HEGame.ui.rasterHeight})),{width:2880,height:1620});
        const tapAction=async action=>{await page.waitForFunction(a=>HEGame.ui.buttons.some(b=>b.action===a),action);await page.evaluate(a=>{const {buttons,layout}=HEGame.ui,b=buttons.find(b=>b.action===a);fixture.tap(layout.left+(b.x+b.w/2)*layout.scale,layout.top+(b.y+b.h/2)*layout.scale);},action);};
        await page.screenshot({path:path.join(out,'landscape-menu.png')});
        await tapAction('friends');await page.waitForTimeout(100);await page.screenshot({path:path.join(out,'landscape-friends.png')});
        await tapAction('friendsClose');await page.waitForTimeout(60);
        await tapAction('start');await page.waitForFunction(()=>HEGame.snapshot.mode==='playing');
        await page.waitForTimeout(1000);await page.screenshot({path:path.join(out,'landscape-game.png')});
        const before=await page.evaluate(()=>({x:HEGame.snapshot.x,z:HEGame.snapshot.z}));
        await page.evaluate(()=>{fixture.events.TouchStart({changedTouches:[{identifier:2,clientX:80,clientY:440}]});fixture.events.TouchMove({changedTouches:[{identifier:2,clientX:125,clientY:440}]});});await page.waitForTimeout(600);
        await page.evaluate(()=>fixture.events.TouchEnd({changedTouches:[{identifier:2}]}));
        assert.ok(await page.evaluate(p=>Math.hypot(HEGame.snapshot.x-p.x,HEGame.snapshot.z-p.z)>.5,before));
        const yaw=await page.evaluate(()=>HEGame.snapshot.cameraYaw);
        await page.evaluate(()=>{
            const e=fixture.events;
            e.TouchStart({changedTouches:[{identifier:3,clientX:90,clientY:430},{identifier:4,clientX:650,clientY:260}]});
            e.TouchMove({changedTouches:[{identifier:3,clientX:130,clientY:430},{identifier:4,clientX:720,clientY:260}]});
        });
        assert.notEqual(await page.evaluate(()=>HEGame.snapshot.cameraYaw),yaw);
        await tapAction('jump');
        await page.waitForTimeout(60);assert.ok(await page.evaluate(()=>HEGame.snapshot.y>0),'jump while moving and looking');
        await page.evaluate(()=>fixture.events.TouchCancel());
        await tapAction('pause');await page.waitForFunction(()=>HEGame.snapshot.mode==='paused');
        await page.screenshot({path:path.join(out,'landscape-paused.png')});
        await tapAction('shared_resume');await page.waitForFunction(()=>HEGame.snapshot.mode==='playing');
        await page.evaluate(()=>fixture.events.Hide());assert.equal(await page.evaluate(()=>HEGame.snapshot.mode),'paused');
        await page.evaluate(()=>fixture.events.Show());await tapAction('shared_resume');
        const roi=await page.evaluate(async()=>{
            const r=HEGame.renderer;await r.ensureModel('green_blob');const state={...HEGame.snapshot,enemies:Array.from({length:200},(_,i)=>({kind:'green_blob',x:500+i,z:500,yaw:0})),pickups:[],projectiles:[]};
            r.frame(state,10);const filtered=r.drawCalls;r.interest.enabled=false;r.frame(state,10);const full=r.drawCalls;r.interest.enabled=true;return {filtered,full};
        });assert.ok(roi.full-roi.filtered>=200);
        await page.evaluate(()=>{fixture.size={width:390,height:844};fixture.events.WindowResize();});await page.setViewportSize({width:390,height:844});await page.waitForTimeout(100);await page.screenshot({path:path.join(out,'portrait-game.png')});
        assert.deepEqual(errors,[]);assert.deepEqual(await page.evaluate(()=>fixture.modals),[]);
        console.log('test-layer PASS: actual exported C# game in simulated WX browser host; landscape menu/start/friend UI/stick/multi-touch look/jump/pause/resume/background, portrait resize, strict WX Module path; distant-actor draw comparison '+JSON.stringify(roi));
    }finally{if(browser)await browser.close();server.kill();}
}
main().catch(error=>{console.error(error);process.exitCode=1;});
