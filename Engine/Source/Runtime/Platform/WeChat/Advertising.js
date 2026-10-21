// Platform lifecycle only. The game decides when to show ads and grant rewards.
export class Advertising {
    constructor(wx,{interstitialId='',rewardedId=''}={}) {Object.assign(this,{wx,interstitialId,rewardedId});this.busy=false;this.instances=new Map();}
    get canReward(){return !!this.rewardedId&&typeof this.wx.createRewardedVideoAd==='function';}
    showInterstitial(){return this.show('interstitial',this.interstitialId);}
    showRewarded(){return this.show('rewarded',this.rewardedId);}
    async show(kind,id){
        if(this.busy)return {status:'busy',completed:false};
        const create=kind==='rewarded'?this.wx.createRewardedVideoAd:this.wx.createInterstitialAd;
        if(!id||typeof create!=='function')return {status:'unavailable',completed:false};
        this.busy=true;
        try{
            let ad=this.instances.get(kind);if(!ad){ad=create.call(this.wx,{adUnitId:id});this.instances.set(kind,ad);}
            return await new Promise(resolve=>{
                let settled=false,shown=false;
                const finish=result=>{if(settled)return;settled=true;clearTimeout(timer);ad.offClose(close);ad.offError(error);resolve(result);};
                const close=result=>finish({status:'closed',completed:kind==='rewarded'&&result?.isEnded===true});
                const error=reason=>finish({status:'failed',completed:false,error:String(reason?.errMsg||reason)});
                // Only time out loading, never grant a reward while a video is still playing.
                const timer=setTimeout(()=>{if(!shown)error('Ad loading timeout');},15000);
                ad.onClose(close);ad.onError(error);
                Promise.resolve().then(()=>ad.show()).catch(()=>{if(!settled)return ad.load().then(()=>{if(!settled)return ad.show();});}).then(()=>{shown=true;clearTimeout(timer);}).catch(error);
            });
        }catch(error){return {status:'failed',completed:false,error:String(error)};}
        finally{this.busy=false;}
    }
}
