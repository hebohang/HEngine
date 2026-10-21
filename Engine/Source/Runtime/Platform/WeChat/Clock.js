'use strict';
// DOM-free millisecond clock for hosts without the browser Performance API.
// Keep this local to each runtime: never replace the game's global objects.
function createClock(host=globalThis, wallNow=()=>Date.now()) {
    const native=host.performance;
    const timeOrigin=wallNow();
    let last=0;
    const read=native&&typeof native.now==='function'?()=>native.now.call(native):()=>wallNow()-timeOrigin;
    return {
        timeOrigin:Number.isFinite(native?.timeOrigin)?native.timeOrigin:timeOrigin,
        now(){const value=read();if(Number.isFinite(value))last=Math.max(last,value);return last;}
    };
}
module.exports={createClock};
