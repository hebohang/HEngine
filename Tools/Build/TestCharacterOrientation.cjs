'use strict';
const {chromium}=require('playwright'),{spawn}=require('node:child_process'),path=require('node:path'),fs=require('node:fs'),assert=require('node:assert/strict');
(async()=>{
    const out=path.resolve('out/agent/model-orientation');fs.mkdirSync(out,{recursive:true});
    const server=spawn(process.execPath,[path.join(__dirname,'ServeWeb.cjs'),'Project/Dist/Web','0'],{windowsHide:true,stdio:['ignore','pipe','pipe']});let browser;
    try{
        const url=await new Promise(resolve=>server.stdout.on('data',b=>{const m=String(b).match(/http:\/\/127\.0\.0\.1:\d+\//);if(m)resolve(m[0]);}));
        browser=await chromium.launch({headless:true,executablePath:'C:/Program Files/Google/Chrome/Application/chrome.exe'});
        for(const backend of ['webgl2','webgpu','compatibility']){
            const page=await browser.newPage({viewport:{width:1200,height:660}}),errors=[];page.on('pageerror',e=>errors.push(String(e)));
            await page.route('**/__characters',r=>r.fulfill({contentType:'text/html',body:'<style>body{margin:0;background:#172638}#render{position:absolute;left:-300px;width:240px;height:290px}</style><canvas id="render"></canvas><canvas id="sheet" width="1200" height="660"></canvas>'}));
            await page.goto(url+'__characters');
            await page.evaluate(async backend=>{
                const {SceneRenderer,trs,quaternion}=await import('./engine/SceneRenderer.js');
                const {createBrowserRenderer}=await import('./engine/BrowserRenderer.js');
                const r=await createBrowserRenderer(SceneRenderer,document.querySelector('#render'),{backend:backend==='compatibility'?'webgl2':backend,compatibilityGeometry:backend==='compatibility',maxPixelRatio:1});
                const catalog=await r.json('catalog.json');r.assetNames=Object.keys((await r.json('content-manifest.json')).assets);
                r.models=new Map();r.modelDefinitions=new Map(catalog.characters.map(m=>[m.id,m]));
                for(const definition of catalog.characters)await r.ensureModel(definition.id);
                globalThis.drawCharacters=async(moving,yaw)=>{
                    const sheet=document.querySelector('#sheet'),ctx=sheet.getContext('2d');ctx.fillStyle='#172638';ctx.fillRect(0,0,1200,660);ctx.font='16px sans-serif';const results=[];
                    for(let i=0;i<catalog.characters.length;i++){
                        const definition=catalog.characters[i],m=r.models.get(definition.id),bones=r.bones(moving?m.run:m.idle,.35);
                        if(!bones||!bones.every(Number.isFinite))throw Error('Invalid animated pose: '+definition.id);
                        for(const heading of [0,Math.PI/2,Math.PI,-Math.PI/2]){
                            const rotation=trs([0,0,0],quaternion(m.upright,heading,0),[1,1,1]);
                            if(rotation[5]<.999)throw Error('Character tipped at heading '+heading+': '+definition.id);
                        }
                        r.beginFrame([0,1.25,3.6],[0,.55,0]);r.draw(m.mesh,trs([0,0,0],quaternion(m.upright,yaw,0),[m.scale,m.scale,m.scale]),null,bones);r.endFrame();
                        const x=(i%5)*240,y=Math.floor(i/5)*330;ctx.drawImage(r.canvas,x,y+30,240,290);ctx.fillStyle='#ffffff';ctx.fillText(definition.id+' / '+(moving?'run':'idle'),x+12,y+22);
                        results.push({id:definition.id,up:m.upright,bones:bones.length/16});
                    }
                    if(r.graphics)await r.graphics.device.queue.onSubmittedWorkDone();
                    const diagnostic=r.diagnostics();if(diagnostic.glError||diagnostic.error)throw Error(JSON.stringify(diagnostic));return results;
                };
            },backend);
            for(const [mode,moving,yaw] of [['idle',false,0],['run-turn',true,Math.PI/3]]){
                const result=await page.evaluate(({moving,yaw})=>drawCharacters(moving,yaw),{moving,yaw});assert.equal(result.length,10);
                await page.locator('#sheet').screenshot({path:path.join(out,backend+'-'+mode+'.png')});
                fs.writeFileSync(path.join(out,backend+'-'+mode+'.json'),JSON.stringify(result,null,2));
            }
            assert.deepEqual(errors,[]);await page.close();
        }
        console.log('runtime-manual-browser PASS: 10 real character meshes, idle/run palettes, four heading checks, WebGL2, WebGPU and mini-game CPU compatibility contact sheets.');
    }finally{await browser?.close();server.kill();}
})().catch(e=>{console.error(e);process.exitCode=1;});
