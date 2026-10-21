'use strict';
const assert=require('node:assert/strict'),vm=require('node:vm'),path=require('node:path'),esbuild=require('../Scripting/node_modules/esbuild');
async function main(){
    const window=new EventTarget(),document=new EventTarget();document.hidden=false;
    const scope={window,document,AbortController};
    const code=esbuild.buildSync({stdin:{contents:"export * from './Engine/Source/Runtime/Platform/Web/Input/BrowserInput.js'; export * from './Engine/Source/Runtime/Platform/Web/BrowserLifecycle.js';",resolveDir:path.resolve(__dirname,'../..')},bundle:true,write:false,format:'iife',globalName:'Host'}).outputFiles[0].text;
    vm.runInNewContext(code,scope);
    const send=(name,data={})=>{const event=new Event(name,{cancelable:true});for(const [key,value]of Object.entries(data))Object.defineProperty(event,key,{value});window.dispatchEvent(event);return event;};
    const target={closest:()=>null},blocked={closest:()=>({})},drags=[];let keyCount=0;
    const input=new scope.Host.BrowserInput({preventKeys:['ArrowUp'],onKeyDown:()=>keyCount++,canDrag:()=>true,canStartDrag:e=>e.clientX>100,onDrag:d=>drags.push(d)});
    assert.equal(send('keydown',{key:'ArrowUp',target}).defaultPrevented,true);assert.ok(input.keys.has('arrowup'));
    send('keydown',{key:'a',target:blocked});assert.equal(keyCount,1);
    send('pointerdown',{pointerType:'touch',pointerId:1,clientX:120,clientY:20,target});
    send('pointermove',{pointerType:'touch',pointerId:2,clientX:90,clientY:30,target});assert.equal(drags.length,0);
    send('pointermove',{pointerType:'touch',pointerId:1,clientX:90,clientY:30,target});assert.equal(drags.length,1);assert.equal(drags[0][0],-30);
    send('blur');assert.equal(input.keys.size,0);send('pointermove',{pointerId:1,clientX:60,clientY:30});assert.equal(drags.length,1);
    input.dispose();send('keydown',{key:'b',target});assert.equal(keyCount,1);
    const calls=[];const dispose=scope.Host.bindBrowserLifecycle({resetInput:()=>calls.push('reset'),onSuspend:()=>calls.push('suspend'),onHidden:()=>calls.push('hidden'),onPageHide:()=>calls.push('save')});
    send('blur');assert.equal(calls.join(','),'reset,suspend');calls.length=0;
    document.hidden=true;document.dispatchEvent(new Event('visibilitychange'));assert.equal(calls.join(','),'reset,suspend,hidden');
    calls.length=0;dispose();send('blur');assert.equal(calls.length,0);
    const element={requestFullscreen:async()=>{document.fullscreenElement=element;}};document.exitFullscreen=async()=>{document.fullscreenElement=null;};
    assert.equal(await scope.Host.toggleFullscreen(element),true);assert.equal(await scope.Host.toggleFullscreen(element),false);
    console.log('unit-test PASS: independent browser input, editable filtering, pointer ownership across zones, blur reset, disposal, lifecycle and fullscreen dispatch');
}
main().catch(error=>{console.error(error);process.exitCode=1;});
