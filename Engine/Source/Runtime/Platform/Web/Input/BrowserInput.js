import {PointerDrag} from './PointerDrag.js';

// DOM input plumbing with no game commands or camera rules.
export class BrowserInput {
    constructor({onKeyDown=()=>{},preventKeys=[],canDrag=()=>false,canStartDrag=canDrag,onDrag=()=>{}}={}){
        this.keys=new Set();this.drag=new PointerDrag();this.abort=new AbortController();
        const listen=(name,callback)=>window.addEventListener(name,callback,{signal:this.abort.signal});
        const blocked=target=>target?.closest?.('button,input,select,textarea,[contenteditable]:not([contenteditable="false"]),[data-he-input-block]');
        listen('keydown',event=>{
            if(event.target?.closest?.('input,select,textarea,[contenteditable]:not([contenteditable="false"])'))return;
            if(preventKeys.includes(event.key))event.preventDefault();
            this.keys.add(event.key.toLowerCase());onKeyDown(event);
        });
        listen('keyup',event=>this.keys.delete(event.key.toLowerCase()));
        listen('pointerdown',event=>{
            if(!blocked(event.target)&&canStartDrag(event)&&(event.pointerType==='touch'||event.button===2))this.drag.begin(event.pointerId,event.clientX,event.clientY);
        });
        listen('pointermove',event=>{
            if(!canDrag(event)){this.drag.reset();return;}
            const delta=this.drag.move(event.pointerId,event.clientX,event.clientY);if(delta)onDrag(delta);
        });
        for(const name of ['pointerup','pointercancel'])listen(name,event=>this.drag.end(event.pointerId));
        listen('contextmenu',event=>{if(!blocked(event.target)&&canDrag(event))event.preventDefault();});
        listen('blur',()=>this.reset());listen('resize',()=>this.reset());
        document.addEventListener('visibilitychange',()=>{if(document.hidden)this.reset();},{signal:this.abort.signal});
    }
    reset(){this.keys.clear();this.drag.reset();}
    dispose(){this.reset();this.abort.abort();}
}
