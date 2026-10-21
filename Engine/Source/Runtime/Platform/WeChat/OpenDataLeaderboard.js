// Runs exclusively in the open data context; only the shared canvas crosses domains.
const canvas=wx.getSharedCanvas(),ctx=canvas.getContext('2d');
let generation=0,writeQueue=Promise.resolve(),layout={width:600,height:400,ratio:1};
function paint(title,message,rows=[],unit=''){
    const {width,height,ratio}=layout;
    ctx.setTransform(1,0,0,1,0,0);ctx.clearRect(0,0,canvas.width,canvas.height);ctx.setTransform(ratio,0,0,ratio,0,0);
    ctx.fillStyle='#102238';ctx.fillRect(0,0,width,height);
    ctx.textAlign='left';ctx.fillStyle='#ffdc89';ctx.font='22px sans-serif';ctx.fillText(title,16,30,width-32);
    ctx.fillStyle='#fff';ctx.font='14px sans-serif';ctx.fillText(message,16,56,width-32);
    rows.forEach((row,i)=>{const y=68+i*44;ctx.fillStyle=i%2?'#19354c':'#22415a';ctx.fillRect(12,y,width-24,40);ctx.fillStyle='#fff';ctx.font='16px sans-serif';ctx.textAlign='left';ctx.fillText(`${row.rank}. ${String(row.name).slice(0,18)}`,22,y+26,width-142);ctx.textAlign='right';ctx.fillText(`${row.score} ${unit}`,width-22,y+26,100);});
    ctx.textAlign='left';
}
const request=(method,options)=>new Promise((resolve,reject)=>wx[method]({...options,success:resolve,fail:reject}));
wx.onMessage(message=>{
    const {type,key}=message;if(typeof key!=='string'||!key)return;
    if(type==='he-leaderboard-score'&&Number.isFinite(message.score)){
        writeQueue=writeQueue.then(async()=>{
            const own=await request('getUserCloudStorage',{keyList:[key]});
            const previous=Number(own.KVDataList?.find(v=>v.key===key)?.value)||0;
            if(message.score>previous)await request('setUserCloudStorage',{KVDataList:[{key,value:String(message.score)}]});
        }).catch(error=>console.warn('[HE leaderboard score]',String(error.errMsg||error)));
    }
    if(type==='he-leaderboard-show'){
        layout={width:Math.max(200,Number(message.width)||600),height:Math.max(120,Number(message.height)||400),ratio:Math.max(1,Math.min(3,Number(message.ratio)||1))};
        canvas.width=Math.round(layout.width*layout.ratio);canvas.height=Math.round(layout.height*layout.ratio);
        const pageSize=Math.max(1,Math.floor((layout.height-76)/44));
        const token=++generation,title=String(message.title||'好友排行榜');paint(title,'正在读取好友成绩…');
        request('getFriendCloudStorage',{keyList:[key]}).then(result=>{
            if(token!==generation)return;
            const rows=result.data.map(friend=>({name:friend.nickname||'微信好友',score:Number(friend.KVDataList?.find(v=>v.key===key)?.value)||0})).sort((a,b)=>b.score-a.score).map((r,i)=>({...r,rank:i+1}));
            const page=Math.min(Math.max(0,Math.floor(message.page||0)),Math.max(0,Math.ceil(rows.length/pageSize)-1));
            paint(title,rows.length?`第 ${page+1} / ${Math.ceil(rows.length/pageSize)} 页 · ${message.metricLabel||'最高成绩'}`:'暂无好友成绩',rows.slice(page*pageSize,page*pageSize+pageSize),String(message.unit||''));
        }).catch(()=>{if(token===generation)paint(title,'暂时无法读取，请确认好友权限后重试');});
    }
});
