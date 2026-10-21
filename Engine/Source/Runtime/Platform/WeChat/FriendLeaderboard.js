// Friend records stay inside WeChat's open data context, never sent to gameplay.
export class FriendLeaderboard {
    constructor(wx,{enabled=false,key='he-score',title='好友排行榜',unit='',metricLabel='最高成绩'}={}){Object.assign(this,{wx,enabled,key,title,unit,metricLabel});this.visible=false;}
    setLayout(width,height,ratio=1){
        const next={width:Math.max(1,Math.round(width)),height:Math.max(1,Math.round(height)),ratio:Math.max(1,Math.min(3,ratio))};
        if(JSON.stringify(next)===JSON.stringify(this.layout))return;
        this.layout=next;
        if(this.visible)this.refresh();
    }
    async show(){
        if(!this.enabled||!this.wx.getOpenDataContext)throw Error('当前环境不支持好友排行榜');
        await new Promise((resolve,reject)=>this.wx.authorize({scope:'scope.WxFriendInteraction',success:resolve,fail:()=>reject(Error('请在微信设置中允许好友互动权限'))}));
        this.context=this.wx.getOpenDataContext();this.visible=true;this.page=0;this.refresh();
    }
    refresh(){
        const {width=600,height=400,ratio=1}=this.layout||{};
        this.context.canvas.width=Math.round(width*ratio);this.context.canvas.height=Math.round(height*ratio);
        this.context.postMessage({type:'he-leaderboard-show',key:this.key,title:this.title,unit:this.unit,metricLabel:this.metricLabel,page:this.page,width,height,ratio});
    }
    next(delta){this.page=Math.max(0,this.page+delta);this.refresh();}
    hide(){this.visible=false;}
    submit(score){if(this.enabled&&Number.isFinite(score)&&score>=0&&this.wx.getOpenDataContext)this.wx.getOpenDataContext().postMessage({type:'he-leaderboard-score',key:this.key,score:Math.floor(score)});}
    composite(overlay,rectangle,viewport){if(this.visible&&rectangle)overlay.drawCanvas(this.context.canvas,...rectangle,viewport.width,viewport.height);}
}
