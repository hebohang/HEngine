// Device-neutral ownership for a look/drag gesture. Hosts provide logical pixels.
export class PointerDrag {
    constructor(){this.reset();}
    reset(){this.pointerId=null;this.x=0;this.y=0;}
    begin(id,x,y){if(this.pointerId!==null||!Number.isFinite(x)||!Number.isFinite(y))return false;this.pointerId=id;this.x=x;this.y=y;return true;}
    move(id,x,y){if(id!==this.pointerId||!Number.isFinite(x)||!Number.isFinite(y))return null;const delta=[x-this.x,y-this.y];this.x=x;this.y=y;return delta;}
    end(id){if(id===this.pointerId)this.reset();}
}
