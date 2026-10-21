import {ViewHost} from '../Web/Portable/ViewHost.js';
import {createPackageAssets} from './PackageAssets.js';
import {applyOrientation,readViewport} from './Viewport.js';

export async function startPortableGame(wx, config, loadRuntime) {
    await applyOrientation(wx,config.orientation);
    await Promise.all(config.subpackages.map(name=>new Promise((resolve,reject)=>wx.loadSubpackage({name,success:resolve,fail:reject}))));
    const canvas=wx.createCanvas(),ctx=canvas.getContext('2d'),viewport=()=>readViewport(wx);
    const assets=createPackageAssets(wx,config.files,viewport);
    const descriptor=await (await assets.fetch('portable-game.json')).json();
    const catalog=await (await assets.fetch('catalog.json')).json();
    const runtime=await loadRuntime();
    const game=(await runtime.getAssemblyExports(runtime.getConfig().mainAssemblyName)).Program;
    const host=new ViewHost(game,{get:key=>wx.getStorageSync(key),set:(key,value)=>wx.setStorageSync(key,value)},descriptor.projectId+'.portable-v1',catalog,path=>{
        const audio=wx.createInnerAudioContext();audio.src=assets.pathFor(path);audio.volume=.25;
        audio.onEnded(()=>audio.destroy());audio.onError(()=>audio.destroy());audio.play();
    });
    const point=t=>[t.clientX??t.x,t.clientY??t.y];
    wx.onTouchStart(e=>{for(const t of e.changedTouches)host.down(t.identifier,...point(t));});
    wx.onTouchMove(e=>{for(const t of e.changedTouches)host.stick.move(t.identifier,...point(t));});
    wx.onTouchEnd(e=>{for(const t of e.changedTouches)host.stick.end(t.identifier);});
    wx.onTouchCancel(()=>host.resetInput());
    let hidden=false,last=Date.now();
    wx.onHide(()=>{hidden=true;host.suspend();});wx.onShow(()=>{hidden=false;last=Date.now();});
    if(wx.onWindowResize)wx.onWindowResize(()=>host.resetInput());
    const root=typeof GameGlobal==='undefined'?globalThis:GameGlobal;
    root.HEGame={get snapshot(){return host.snapshot;},get buttons(){return host.buttons;},get frames(){return host.frames;},command:name=>host.command(name),save:()=>host.session.save(),running:true};
    function frame() {
        const v=viewport(),ratio=Math.min(v.ratio,3),now=Date.now();
        if(canvas.width!==Math.round(v.width*ratio)||canvas.height!==Math.round(v.height*ratio)){canvas.width=Math.round(v.width*ratio);canvas.height=Math.round(v.height*ratio);}
        ctx.setTransform(ratio,0,0,ratio,0,0);
        if(!hidden)host.step(Math.max(0,Math.min(.05,(now-last)/1000)));last=now;host.paint(ctx,v.width,v.height);
        requestAnimationFrame(frame);
    }
    requestAnimationFrame(frame);
    return host;
}
