'use strict';
const path = require('node:path'), assert = require('node:assert/strict'), {pathToFileURL}=require('node:url');
(async()=>{
globalThis.window=new EventTarget();globalThis.document=new EventTarget();
const {VirtualJoystick}=await import(pathToFileURL(path.resolve(__dirname,'../../Engine/Source/Runtime/Platform/Web/Input/VirtualJoystick.js')));
const {PointerDrag}=await import(pathToFileURL(path.resolve(__dirname,'../../Engine/Source/Runtime/Platform/Web/Input/PointerDrag.js')));
const drag=new PointerDrag();assert.equal(drag.begin(1,20,30),true);assert.equal(drag.begin(2,0,0),false);
assert.equal(drag.move(2,40,50),null);assert.deepEqual(drag.move(1,40,50),[20,20]);drag.end(2);assert.equal(drag.pointerId,1);
assert.equal(drag.move(1,NaN,0),null);drag.end(1);assert.equal(drag.pointerId,null);
const context={VirtualJoystick,window,document};
class Surface extends EventTarget {
    constructor() { super(); this.firstElementChild = { style: {} }; this.captures = new Set(); }
    getBoundingClientRect() { return { left: 0, top: 0, width: 100, height: 100 }; }
    setPointerCapture(id) { this.captures.add(id); }
    hasPointerCapture(id) { return this.captures.has(id); }
    releasePointerCapture(id) { this.captures.delete(id); }
}
function send(target, type, x = 50, y = 50, id = 1) {
    const event = new Event(type, { cancelable: true }); Object.assign(event, { clientX: x, clientY: y, pointerId: id, button: 0 }); target.dispatchEvent(event);
}
const surface = new Surface(); let changes = 0;
const stick = new context.VirtualJoystick(surface, { onChange: () => changes++ });
send(surface, 'pointerdown', 52, 50); assert.equal(stick.x, 0, 'dead zone');
send(surface, 'pointermove', 150, 150); assert.ok(Math.abs(Math.hypot(stick.x, stick.y) - 1) < 1e-9, 'radial clamping');
send(surface, 'pointerdown', 0, 50, 2); send(surface, 'pointerup', 0, 50, 2); assert.equal(stick.pointerId, 1, 'second pointer must not steal or release');
send(surface, 'pointercancel'); assert.equal(stick.x, 0); assert.equal(surface.captures.size, 0);
send(surface, 'pointerdown', 0, 50); send(surface, 'lostpointercapture'); assert.equal(stick.pointerId, null);
send(surface, 'pointerdown', 90, 50); send(context.window, 'blur'); assert.equal(stick.x, 0, 'blur reset');
send(surface, 'pointerdown', 90, 50); context.document.hidden = true; send(context.document, 'visibilitychange'); assert.equal(stick.x, 0, 'hidden reset');
stick.dispose(); const count = changes; send(surface, 'pointerdown', 90, 50); assert.equal(changes, count, 'disposed listeners');
assert.throws(() => new context.VirtualJoystick(surface, { radius: 0 }), RangeError);
console.log('unit-test PASS: dead-zone, radial-clamp, pointer-isolation, cancel, lost-capture, blur, visibility, dispose, invalid-options');
})().catch(error=>{console.error(error);process.exitCode=1;});
