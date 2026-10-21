'use strict';
const {spawn}=require('node:child_process'),path=require('node:path'),fs=require('node:fs'),assert=require('node:assert/strict');
const {chromium}=require('playwright');
async function main(){
    const root=path.resolve(__dirname,'../..'),out=path.join(root,'out/agent/browser-renderers');fs.mkdirSync(out,{recursive:true});
    const server=spawn(process.execPath,[path.join(__dirname,'ServeWeb.cjs'),root,'0'],{windowsHide:true,stdio:['ignore','pipe','pipe']});let browser;
    try{
        const url=await new Promise((resolve,reject)=>{server.stdout.on('data',b=>{const m=String(b).match(/http:\/\/127\.0\.0\.1:\d+\//);if(m)resolve(m[0]);});server.on('error',reject);});
        browser=await chromium.launch({headless:true,executablePath:process.env.HE_SCRIPT_CHROME||'C:/Program Files/Google/Chrome/Application/chrome.exe'});
        const page=await browser.newPage({viewport:{width:800,height:600}}),errors=[];page.on('pageerror',e=>errors.push(String(e)));
        await page.route('**/__renderers',r=>r.fulfill({contentType:'text/html',body:'<!doctype html><title>Render backend validation</title>'}));await page.goto(url+'__renderers');
        const result=await page.evaluate(async()=>{
            const base='/Engine/Source/Runtime/Platform/Web/';
            const {SceneRenderer,trs}=await import(base+'SceneRenderer.js');
            const {createBrowserRenderer,recoverBrowserRenderer}=await import(base+'BrowserRenderer.js');
            const originalParameter=WebGL2RenderingContext.prototype.getParameter;
            let noVertexTextures=false;
            WebGL2RenderingContext.prototype.getParameter=function(p){
                if(p===this.MAX_VERTEX_UNIFORM_VECTORS)return 256;
                if(p===this.MAX_VERTEX_TEXTURE_IMAGE_UNITS&&noVertexTextures)return 0;
                return originalParameter.call(this,p);
            };
            const canvas=()=>{const c=document.createElement('canvas');c.style.width='256px';c.style.height='256px';document.body.appendChild(c);return c;};
            const identity=()=>trs([0,0,0],[0,0,0,1],[1,1,1]);
            const pixels=renderer=>{const c=document.createElement('canvas');c.width=c.height=256;const ctx=c.getContext('2d');ctx.drawImage(renderer.canvas,0,0);return ctx.getImageData(0,0,256,256).data;};
            const center=data=>{let sum=0,count=0;for(let y=0;y<256;y++)for(let x=0;x<256;x++){const i=(y*256+x)*4;if(data[i+1]>data[i]*1.5&&data[i+1]>70){sum+=x;count++;}}if(count<100)throw Error('Rendered triangle missing');return sum/count;};
            const results=[],images=[];
            for(const mode of ['texture','cpu','webgpu']){
                noVertexTextures=mode==='cpu';
                const r=await createBrowserRenderer(SceneRenderer,canvas(),{backend:mode==='webgpu'?'webgpu':'webgl2',maxPixelRatio:1});
                const mesh={parts:[{...r.upload(new Float32Array([-.8,-.6,0,.8,-.6,0,0,.8,0]),new Float32Array([0,0,1,0,0,1,0,0,1]),new Float32Array(6),new Uint16Array([0,1,2]),new Float32Array([127,0,0,0,127,0,0,0,127,0,0,0]),new Float32Array([1,0,0,0,1,0,0,0,1,0,0,0])),texture:r.white,color:[.1,1,.2,1]}]};
                const palette=new Float32Array(128*16);for(let i=0;i<128;i++)palette.set(identity(),i*16);
                async function render(bones){r.beginFrame([0,0,4],[0,0,0]);r.draw(mesh,identity(),null,bones);r.endFrame();const data=pixels(r);if(r.graphics)await r.graphics.device.queue.onSubmittedWorkDone();return data;}
                const first=await render(palette);const changed=palette.slice();changed[127*16+12]=.6;const second=await render(changed);
                let shift;try{shift=center(second)-center(first);}catch(e){throw Error(mode+': '+e.message+' diagnostics='+JSON.stringify(r.diagnostics())+' firstPixel='+Array.from(first.slice(131584,131588)));}if(shift<25)throw Error(mode+': bone 127 did not animate');
                const restored=await render(null);if(Math.abs(center(restored)-center(first))>1)throw Error(mode+': bind pose not restored');
                const diagnostics=r.diagnostics();if(diagnostics.error||diagnostics.glError)throw Error(JSON.stringify(diagnostics));
                if(r.gl){
                    const gl=r.gl;let budget=0;
                    for(let i=0;i<gl.getProgramParameter(r.program,gl.ACTIVE_UNIFORMS);i++){const u=gl.getActiveUniform(r.program,i);budget+=u.size*(u.type===gl.FLOAT_MAT4?4:1);}
                    if(budget>256)throw Error('Shader still exceeds the mobile uniform budget');diagnostics.activeUniformVectorBudget=budget;
                }
                // Same mesh, two models/poses/colors in one submission: later
                // writes must not change the first draw's uniforms or bones.
                r.beginFrame([0,0,4],[0,0,0]);
                r.draw(mesh,trs([-.8,0,0],[0,0,0,1],[.4,.4,.4]),[1,.1,.1,1],palette);
                r.draw(mesh,trs([.6,0,0],[0,0,0,1],[.4,.4,.4]),[.1,1,.1,1],changed);r.endFrame();
                const pair=pixels(r);let reds=0,greens=0;
                for(let y=0;y<256;y++)for(let x=0;x<256;x++){const i=(y*256+x)*4;if(x<128&&pair[i]>100&&pair[i]>pair[i+1]*2)reds++;if(x>128&&pair[i+1]>100&&pair[i+1]>pair[i]*2)greens++;}
                if(reds<30||greens<30)throw Error(mode+': per-draw data aliased');
                results.push({mode,shift,diagnostics});images.push(Array.from(second));
                if(mode==='webgpu'){
                    // Destruction resolves device.lost; recovery must use a NEW canvas.
                    r.graphics.device.destroy();await r.graphics.device.lost;await Promise.resolve();
                    let loss;try{r.beginFrame([0,0,4],[0,0,0]);}catch(e){loss=e;}
                    if(!loss?.renderBackendFailure)throw Error('Device loss not surfaced');
                    const replacement=await recoverBrowserRenderer(SceneRenderer,r,async()=>{},loss);
                    if(replacement.canvas===r.canvas||replacement.diagnostics().backend!=='webgl2')throw Error('Device loss fallback failed');
                    results.push({mode:'device-loss',backend:replacement.diagnostics().backend});
                }
            }
            // Compare actual rendered pixels, not merely API calls.
            for(let i=1;i<images.length;i++){
                let delta=0;for(let j=0;j<images[0].length;j++)delta+=Math.abs(images[0][j]-images[i][j]);
                const mean=delta/images[0].length;if(mean>3)throw Error('Backend image mismatch: '+mean);results[i].meanPixelDifference=mean;
            }
            Object.defineProperty(navigator,'gpu',{configurable:true,value:undefined});
            const fallback=await createBrowserRenderer(SceneRenderer,canvas());
            if(fallback.diagnostics().backend!=='webgl2'||!fallback.fallbackReason)throw Error('Missing API fallback failed');
            results.push({mode:'missing-api',backend:fallback.diagnostics().backend});delete navigator.gpu;
            // Adapter/device failures must leave the original canvas available.
            Object.defineProperty(navigator,'gpu',{configurable:true,value:{requestAdapter:async()=>({requestDevice:async()=>{throw Error('fixture device denial');}})}});
            const denied=await createBrowserRenderer(SceneRenderer,canvas());if(!denied.fallbackReason.includes('denial'))throw Error('Device rejection not diagnosed');
            results.push({mode:'device-denied',backend:denied.diagnostics().backend});delete navigator.gpu;
            WebGL2RenderingContext.prototype.getParameter=originalParameter;return results;
        });
        assert.deepEqual(errors,[]);fs.writeFileSync(path.join(out,'results.json'),JSON.stringify(result,null,2));
        await page.screenshot({path:path.join(out,'backends.png')});console.log('runtime-manual-browser PASS: '+JSON.stringify(result));
    }finally{if(browser)await browser.close();server.kill();}
}
main().catch(e=>{console.error(e);process.exitCode=1;});
