// Mini-game audio host. The project provides logical cues; paths come from its package manifest.
export class AudioBank {
    constructor(wx, paths, resolve) { this.wx=wx;this.paths=paths;this.resolve=resolve;this.effects=new Map();this.music=null;this.track=''; }
    create(name, loop=false) {
        if(!this.paths[name])return null;
        const audio=this.wx.createInnerAudioContext();audio.src=this.resolve(this.paths[name]);audio.loop=loop;audio.volume=loop ? .3 : .65;
        audio.onError(error=>console.warn('[Audio]',name,error.errMsg));return audio;
    }
    play(name) { let audio=this.effects.get(name);if(!audio){audio=this.create(name);if(!audio)return;this.effects.set(name,audio);}audio.stop();audio.play(); }
    setMusic(name) {if(this.track===name)return;this.music?.destroy();this.music=this.create(name,true);this.track=name;this.music?.play();}
    pause() {this.music?.pause();for(const audio of this.effects.values())audio.stop();}
    resume() {this.music?.play();}
    dispose() {this.music?.destroy();for(const audio of this.effects.values())audio.destroy();this.effects.clear();}
}
