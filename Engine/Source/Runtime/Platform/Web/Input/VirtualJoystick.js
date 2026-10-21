import { AnalogStick } from './AnalogStick.js';
/** DOM binding for the shared pointer input used by browser and mini-game hosts. */
export class VirtualJoystick {
    constructor(element, { knob = element.firstElementChild, radius = 40, autoRadius = false, deadZone = 0.1, onChange = () => {} } = {}) {
        if (!(radius > 0) || !(deadZone >= 0 && deadZone < 1)) throw new RangeError('Invalid joystick range');
        this.element = element; this.knob = knob; this.radius = radius; this.deadZone = deadZone;
        this.onChange = onChange; this.pointerId = null; this.x = this.y = 0;
        this.autoRadius = autoRadius;
        this.stick = new AnalogStick({ radius, deadZone });
        this.abort = new AbortController();
        const listen = (target, name, callback) => target.addEventListener(name, callback, { signal: this.abort.signal });
        listen(element, 'pointerdown', event => {
            if (this.pointerId !== null || event.button > 0) return;
            event.preventDefault(); this.pointerId = event.pointerId;
            const rect = element.getBoundingClientRect();
            if(this.autoRadius)this.radius=this.stick.radius=Math.max(1,Math.min(rect.width,rect.height)/2);
            this.stick.begin(event.pointerId, rect.left + rect.width / 2, rect.top + rect.height / 2);
            element.setPointerCapture(event.pointerId); this.move(event);
        });
        listen(element, 'pointermove', event => { if (event.pointerId === this.pointerId) this.move(event); });
        for (const name of ['pointerup', 'pointercancel', 'lostpointercapture'])
            listen(element, name, event => { if (event.pointerId === this.pointerId) this.reset(); });
        listen(window, 'blur', () => this.reset());
        listen(window, 'resize', () => this.reset());
        listen(document, 'visibilitychange', () => { if (document.hidden) this.reset(); });
    }
    move(event) {
        this.stick.move(event.pointerId, event.clientX, event.clientY);
        this.x = this.stick.x; this.y = this.stick.y;
        this.update();
    }
    update() {
        if (this.knob) this.knob.style.transform = `translate(${this.x * this.radius * .8}px, ${this.y * this.radius * .8}px)`;
        this.onChange(this.x, this.y);
    }
    reset() {
        const id = this.pointerId; this.pointerId = null;
        this.stick.reset();
        if (id !== null && this.element.hasPointerCapture(id)) this.element.releasePointerCapture(id);
        this.x = this.y = 0; this.update();
    }
    dispose() { this.reset(); this.abort.abort(); }
}
