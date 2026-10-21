// Visibility/streaming policy only; never despawn or stop authoritative gameplay.
export class InterestRegion {
    constructor({enabled=false,radius=65,padding=25,animationHz=20}={}) {
        if(!(radius>0)||!(padding>=0)||!(animationHz>0))throw Error('Invalid interest region');
        Object.assign(this,{enabled,radius,padding,animationHz});
    }
    contains(x,z,centerX,centerZ,margin=0){return !this.enabled||(x-centerX)**2+(z-centerZ)**2<=(this.radius+margin)**2;}
    animationTime(seconds){return this.enabled?Math.floor(seconds*this.animationHz)/this.animationHz:seconds;}
}
