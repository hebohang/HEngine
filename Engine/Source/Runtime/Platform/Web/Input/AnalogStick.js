// Host-independent stick math; platforms only translate their pointer events.
export class AnalogStick {
    constructor({radius=56,deadZone=.12}={}) {
        if (!(radius>0) || !Number.isFinite(radius) || !(deadZone>=0&&deadZone<1)) throw Error('Invalid stick configuration');
        this.radius=radius;this.deadZone=deadZone;this.reset();
    }
    begin(id,x,y) {
        if(this.id!==null)return false;
        this.id=id;this.originX=x;this.originY=y;this.x=this.y=0;return true;
    }
    move(id,x,y) {
        if(this.id!==id)return false;
        const dx=(x-this.originX)/this.radius,dy=(y-this.originY)/this.radius,length=Math.hypot(dx,dy);
        const magnitude=Math.max(0,Math.min(1,(length-this.deadZone)/(1-this.deadZone)));
        this.x=length?dx/length*magnitude:0;this.y=length?dy/length*magnitude:0;return true;
    }
    end(id) {if(this.id!==id)return false;this.reset();return true;}
    reset() {this.id=null;this.x=this.y=0;this.originX=this.originY=0;}
}
