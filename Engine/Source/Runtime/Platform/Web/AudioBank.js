// Browser-only audio transport. Resource paths come from the game's audio bank.
export class AudioBank {
    constructor(bank){this.bank=bank;this.buffers=new Map();this.context=null;this.music=null;this.track='';this.cooldowns=new Map();}
    async unlock(){
        if(!this.context){this.context=new AudioContext();this.gain=this.context.createGain();this.gain.gain.value=.5;this.gain.connect(this.context.destination);}
        await this.context.resume();
    }
    async buffer(id){if(!this.buffers.has(id))this.buffers.set(id,fetch(this.bank[id]).then(r=>{if(!r.ok)throw Error('Audio asset missing: '+id);return r.arrayBuffer();}).then(b=>this.context.decodeAudioData(b)));return this.buffers.get(id);}
    async play(id,volume=.5){if(!this.context||this.context.state!=='running'||!this.bank[id])return;const now=performance.now();if(now-(this.cooldowns.get(id)||0)<80)return;this.cooldowns.set(id,now);
        const buffer=await this.buffer(id),source=this.context.createBufferSource(),gain=this.context.createGain();gain.gain.value=volume;source.buffer=buffer;source.connect(gain);gain.connect(this.gain);source.onended=()=>{source.disconnect();gain.disconnect();};source.start();}
    async setMusic(id){if(!this.context||this.track===id)return;this.track=id;const buffer=await this.buffer(id);if(this.track!==id)return;if(this.music){this.music.stop();this.music.disconnect();}const source=this.context.createBufferSource(),gain=this.context.createGain();gain.gain.value=.36;source.buffer=buffer;source.loop=true;source.connect(gain);gain.connect(this.gain);source.start();this.music=source;}
    pause(){if(this.context)this.context.suspend();}
}
