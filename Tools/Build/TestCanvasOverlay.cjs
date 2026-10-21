'use strict';
const {spawn}=require('node:child_process'),path=require('node:path'),assert=require('node:assert/strict');
const {chromium}=require('playwright');
async function main(){
    const root=path.resolve(__dirname,'../..'),server=spawn(process.execPath,[path.join(__dirname,'ServeWeb.cjs'),root,'0'],{windowsHide:true,stdio:['ignore','pipe','pipe']});let browser;
    try{
        const url=await new Promise((resolve,reject)=>{server.stdout.on('data',b=>{const m=String(b).match(/http:\/\/127\.0\.0\.1:\d+\//);if(m)resolve(m[0]);});server.on('error',reject);});
        browser=await chromium.launch({headless:true,executablePath:process.env.HE_SCRIPT_CHROME||'C:/Program Files/Google/Chrome/Application/chrome.exe'});
        const page=await browser.newPage();await page.route('**/__overlaytest',route=>route.fulfill({contentType:'text/html',body:'<!doctype html><title>Overlay test</title>'}));await page.goto(url+'__overlaytest');
        const result=await page.evaluate(async()=>{
            const {SceneRenderer,trs}=await import('/Engine/Source/Runtime/Platform/Web/SceneRenderer.js');
            const {CanvasOverlay}=await import('/Engine/Source/Runtime/Platform/Web/CanvasOverlay.js');
            const {LoadingScreen}=await import('/Engine/Source/Runtime/Platform/Web/LoadingScreen.js');
            const canvas=document.createElement('canvas');document.body.appendChild(canvas);let size=128;
            const renderer=new SceneRenderer(canvas,{compatibilityGeometry:true,viewport:()=>({width:size,height:size,ratio:1})}),gl=renderer.gl;
            const overlay=new CanvasOverlay(gl,document.createElement('canvas'),{pixelUpload:true});
            // An explicit albedo texture must remain visible behind transparent UI.
            const texture=gl.createTexture();gl.bindTexture(gl.TEXTURE_2D,texture);gl.texImage2D(gl.TEXTURE_2D,0,gl.RGBA,1,1,0,gl.RGBA,gl.UNSIGNED_BYTE,new Uint8Array([30,230,60,255]));renderer.primitive.parts[0].texture=texture;
            const original=renderer.primitive.parts[0],vertices=original.position.length/3,joints=new Float32Array(vertices*4),weights=new Float32Array(vertices*4);for(let i=0;i<vertices;i++)weights[i*4]=1;
            const skinned={parts:[{...renderer.upload(original.position,original.normal,new Float32Array(vertices*2),Array.from({length:vertices},(_,i)=>i),joints,weights),texture,color:[1,1,1,1]}]};
            const pose=new Float32Array([1,0,0,0,0,1,0,0,0,0,1,0,.1,0,0,1]);
            const sample=(x,y)=>{const p=new Uint8Array(4);gl.readPixels(x,y,1,1,gl.RGBA,gl.UNSIGNED_BYTE,p);return Array.from(p);};
            const checks=[];
            for(size of [128,256]){
                gl.disable(gl.BLEND);gl.depthMask(false);gl.depthFunc(gl.NEVER);gl.enable(gl.SCISSOR_TEST);gl.scissor(0,0,1,1);
                renderer.beginFrame([0,1,4],[0,0,0]);renderer.draw(skinned,trs([0,0,0],[0,0,0,1],[1,1,1]),null,pose);renderer.endFrame();
                if(Math.abs(skinned.parts[0].lastBones[12]-.1)>.001)throw Error('CPU bone pose not used');
                const before=sample(size/2,size/2);
                gl.disable(gl.BLEND);gl.enable(gl.SCISSOR_TEST);gl.scissor(0,0,1,1);
                overlay.draw(size,size,(ctx)=>{ctx.fillStyle='#ffdd00';ctx.fillRect(4,4,24,24);});
                const after=sample(size/2,size/2),hud=sample(10,size-10);
                if(before[1]<before[0]*2||before[1]<80)throw Error('Textured scene did not render');
                if(String(before)!==String(after))throw Error('Transparent HUD overwrote the scene');
                if(hud[0]<240||hud[1]<200)throw Error('HUD did not render');
                const layer=document.createElement('canvas');layer.width=layer.height=16;const layerCtx=layer.getContext('2d');layerCtx.fillStyle='#0000ff';layerCtx.fillRect(0,0,16,16);overlay.drawCanvas(layer,32,32,16,16,size,size);if(sample(40,size-40)[2]!==255)throw Error('Direct canvas layer missing');
                checks.push({size,before,after,hud});
            }
            const loading=new LoadingScreen('Fixture');loading.update('Loading textures',.5);overlay.draw(size,size,(...args)=>loading.paint(...args));
            if(gl.getError()!==gl.NO_ERROR)throw Error('Loading screen GL error');
            return checks;
        });
        assert.equal(result.length,2);console.log('runtime-manual-browser PASS: textured 3D survives RGBA HUD, HUD visible, state contamination and resize recovered, loading screen renders; '+JSON.stringify(result));
    }finally{if(browser)await browser.close();server.kill();}
}
main().catch(e=>{console.error(e);process.exitCode=1;});
