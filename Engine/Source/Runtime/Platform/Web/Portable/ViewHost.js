import {ManagedGameSession} from '../ManagedGameSession.js';
import {AnalogStick} from '../Input/AnalogStick.js';

export function validateView(s) {
    if(!s || s.schema!==1 || !Array.isArray(s.objects) || !Array.isArray(s.actions) || s.objects.length>256 || s.actions.length>4 ||
        !Number.isFinite(s.worldWidth) || !Number.isFinite(s.worldHeight) || s.worldWidth<=0 || s.worldHeight<=0)
        throw Error('Unsupported portable view schema/bounds/budget');
    const ids=new Set();
    for(const o of s.objects) {
        if(!o || typeof o.id!=='string' || !o.id || ids.has(o.id) || ![o.x,o.y,o.width,o.height].every(Number.isFinite) ||
            o.width<=0 || o.height<=0 || typeof o.color!=='string' || !/^#[0-9a-f]{6}$/i.test(o.color))
            throw Error('Invalid portable object ID/geometry/color');
        ids.add(o.id);
    }
    ids.clear();
    for(const a of s.actions) {
        if(!a || typeof a.id!=='string' || !a.id || ids.has(a.id) || typeof a.text!=='string')throw Error('Invalid portable action');
        ids.add(a.id);
    }
}

// Rectangular 2D view protocol. Rules, hits, spawning and outcomes stay in C#.
export class ViewHost {
    constructor(game, storage, key, catalog, sound = () => {}) {
        this.session = new ManagedGameSession(game, storage, key);
        this.snapshot = this.session.initialize(catalog);
        validateView(this.snapshot);
        this.stick = new AnalogStick(); this.keys = new Set(); this.buttons = [];
        this.sound = sound; this.soundRevision = this.snapshot.soundRevision;
        this.frames = 0;
    }
    update(snapshot) {
        validateView(snapshot);
        if(snapshot.soundRevision!==this.soundRevision) {
            this.soundRevision=snapshot.soundRevision;
            if(snapshot.sound) this.sound(snapshot.sound);
        }
        this.snapshot=snapshot;
    }
    command(name) { this.update(this.session.command(name)); this.resetInput(); }
    step(seconds) {
        const x=this.stick.x + (this.keys.has('d')||this.keys.has('arrowright')?1:0) - (this.keys.has('a')||this.keys.has('arrowleft')?1:0);
        const y=this.stick.y + (this.keys.has('s')||this.keys.has('arrowdown')?1:0) - (this.keys.has('w')||this.keys.has('arrowup')?1:0);
        this.update(this.session.step(seconds,x,y)); ++this.frames;
    }
    resetInput() { this.stick.reset(); this.keys.clear(); }
    suspend() { this.resetInput(); if(this.snapshot.mode==='playing')this.command('pause'); this.session.persist(); }
    down(id,x,y) {
        const button=this.buttons.find(b=>x>=b.x&&x<=b.x+b.width&&y>=b.y&&y<=b.y+b.height);
        if(button) { this.command(button.id); return; }
        if(this.snapshot.mode==='playing')this.stick.begin(id,x,y);
    }
    paint(ctx,width,height) {
        const s=this.snapshot;
        ctx.fillStyle='#122230';ctx.fillRect(0,0,width,height);
        const scale=Math.max(1,Math.min((width-32)/s.worldWidth,(height-130)/s.worldHeight));
        const cx=width/2,cy=height/2+5;
        ctx.fillStyle='#203746';ctx.fillRect(cx-s.worldWidth/2*scale,cy-s.worldHeight/2*scale,s.worldWidth*scale,s.worldHeight*scale);
        for(const o of s.objects) {
            ctx.fillStyle=o.color;ctx.fillRect(cx+(o.x-o.width/2)*scale,cy+(o.y-o.height/2)*scale,o.width*scale,o.height*scale);
        }
        ctx.textBaseline='top';ctx.textAlign='center';ctx.fillStyle='#e8f6ff';ctx.font='bold 22px sans-serif';ctx.fillText(s.title,width/2,14);
        ctx.font='14px sans-serif';ctx.fillStyle='#b5cedd';ctx.fillText(s.status,width/2,44,width-24);
        this.buttons=[];
        const gap=12,bw=Math.min(190,(width-32)/Math.max(1,s.actions.length)-gap),total=s.actions.length*(bw+gap)-gap;
        s.actions.forEach((a,i)=>{
            const b={id:a.id,x:(width-total)/2+i*(bw+gap),y:height-52,width:bw,height:38};this.buttons.push(b);
            ctx.fillStyle='#80ed99';ctx.fillRect(b.x,b.y,b.width,b.height);
            ctx.fillStyle='#122230';ctx.font='bold 16px sans-serif';ctx.fillText(a.text,b.x+b.width/2,b.y+9,b.width-8);
        });
        if(this.stick.id!==null) {
            ctx.strokeStyle='#b5cedd';ctx.lineWidth=2;ctx.beginPath();ctx.arc(this.stick.originX,this.stick.originY,this.stick.radius,0,Math.PI*2);ctx.stroke();
            ctx.fillStyle='#80ed9977';ctx.beginPath();ctx.arc(this.stick.originX+this.stick.x*this.stick.radius,this.stick.originY+this.stick.y*this.stick.radius,16,0,Math.PI*2);ctx.fill();
        }
    }
}
