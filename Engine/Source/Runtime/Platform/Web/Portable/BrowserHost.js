import {ViewHost} from './ViewHost.js';

export async function startPortableGame() {
    const canvas=document.querySelector('canvas'),ctx=canvas.getContext('2d');
    const descriptor=await (await fetch('./portable-game.json')).json();
    const catalog=await (await fetch('./catalog.json')).json();
    const {dotnet}=await import('../../_framework/dotnet.js');
    const runtime=await dotnet.create();
    const game=(await runtime.getAssemblyExports(runtime.getConfig().mainAssemblyName)).Program;
    const host=new ViewHost(game,{get:key=>localStorage.getItem(key),set:(key,value)=>localStorage.setItem(key,value)},descriptor.projectId+'.portable-v1',catalog,
        path=>{const audio=new Audio(path);audio.volume=.25;audio.play().catch(()=>{});});
    document.title=descriptor.title;
    const expose={get snapshot(){return host.snapshot;},get frames(){return host.frames;},get buttons(){return host.buttons;},command:name=>host.command(name),save:()=>host.session.save(),running:true};
    globalThis.HEGame=expose;
    const point=e=>{const r=canvas.getBoundingClientRect();return [e.clientX-r.left,e.clientY-r.top];};
    canvas.addEventListener('pointerdown',e=>{canvas.setPointerCapture(e.pointerId);host.down(e.pointerId,...point(e));});
    canvas.addEventListener('pointermove',e=>host.stick.move(e.pointerId,...point(e)));
    for(const name of ['pointerup','pointercancel'])canvas.addEventListener(name,e=>host.stick.end(e.pointerId));
    window.addEventListener('keydown',e=>{
        const key=e.key.toLowerCase();
        if(['arrowup','arrowdown','arrowleft','arrowright',' ','escape'].includes(key))e.preventDefault();
        if(e.repeat)return;
        if(key==='enter'&&host.snapshot.actions[0])host.command(host.snapshot.actions[0].id);
        else if(key==='escape'&&['playing','paused'].includes(host.snapshot.mode))host.command(host.snapshot.mode==='playing'?'pause':'resume');
        else host.keys.add(key);
    });
    window.addEventListener('keyup',e=>host.keys.delete(e.key.toLowerCase()));
    window.addEventListener('blur',()=>host.suspend());
    window.addEventListener('pagehide',()=>host.suspend());
    document.addEventListener('visibilitychange',()=>{if(document.hidden)host.suspend();});
    window.addEventListener('resize',()=>host.resetInput());
    let last=performance.now();
    function frame(now) {
        const width=window.innerWidth,height=window.innerHeight,ratio=Math.min(devicePixelRatio||1,3);
        if(canvas.width!==Math.round(width*ratio)||canvas.height!==Math.round(height*ratio)){canvas.width=Math.round(width*ratio);canvas.height=Math.round(height*ratio);}
        ctx.setTransform(ratio,0,0,ratio,0,0);
        if(!document.hidden)host.step(Math.max(0,Math.min(.05,(now-last)/1000)));
        last=now;host.paint(ctx,width,height);requestAnimationFrame(frame);
    }
    requestAnimationFrame(frame);
}
