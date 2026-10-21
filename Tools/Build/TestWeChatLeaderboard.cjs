'use strict';
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),vm=require('node:vm');
const root=path.resolve(__dirname,'../..'),esbuild=require('../Scripting/node_modules/esbuild');
const code=esbuild.buildSync({entryPoints:[path.join(root,'Engine/Source/Runtime/Platform/WeChat/FriendLeaderboard.js')],bundle:true,write:false,format:'iife',globalName:'Board'}).outputFiles[0].text;
const scope={};vm.runInNewContext(code,scope);
async function main(){
    const messages=[],canvas={width:0,height:0},host={authorize:({success})=>success(),getOpenDataContext:()=>({canvas,postMessage:m=>messages.push(m)})};
    const board=new scope.Board.FriendLeaderboard(host,{enabled:true});
    board.setLayout(544,176,3);await board.show();
    assert.equal(canvas.width,1632);assert.equal(canvas.height,528);
    assert.equal(messages.length,1);board.setLayout(544,176,3);assert.equal(messages.length,1,'same layout does not clear shared canvas');
    board.next(1);assert.equal(messages.at(-1).page,1);
    board.setLayout(342,640,2);assert.equal(canvas.width,684);assert.equal(canvas.height,1280);assert.equal(messages.at(-1).width,342);
    let onMessage;const requests=[],rects=[],texts=[];
    const ctx={setTransform(){},clearRect(){rects.length=0;texts.length=0;},fillRect:(...r)=>rects.push(r),fillText:(...t)=>texts.push(t)};
    const shared={width:0,height:0,getContext:()=>ctx};
    vm.runInNewContext(fs.readFileSync(path.join(root,'Engine/Source/Runtime/Platform/WeChat/OpenDataLeaderboard.js'),'utf8'),{console,wx:{getSharedCanvas:()=>shared,onMessage:fn=>onMessage=fn,getFriendCloudStorage:options=>requests.push(options)}});
    const friends=Array.from({length:18},(_,i)=>({nickname:'好友 '+i,KVDataList:[{key:'test',value:String(100-i)}]}));
    for(const [width,height,ratio] of [[544,176,3],[600,300,3],[342,640,2]]){
        onMessage({type:'he-leaderboard-show',key:'test',width,height,ratio,page:1});
        requests.at(-1).success({data:friends});await Promise.resolve();await Promise.resolve();
        assert.equal(shared.width,width*ratio);assert.equal(shared.height,height*ratio);
        const count=Math.floor((height-76)/44);
        assert.equal(rects.length-1,Math.min(count,18-count),'row count adapts to height');
        for(const [x,y,w,h] of rects)assert.ok(x>=0&&y>=0&&x+w<=width&&y+h<=height,'rows stay inside their logical viewport');
        assert.ok(texts.some(t=>String(t[0]).startsWith(`${count+1}. `)),'page starts at the responsive row offset');
    }
    onMessage({type:'he-leaderboard-show',key:'test',width:544,height:176,page:0});
    const stale=requests.at(-1);
    onMessage({type:'he-leaderboard-show',key:'test',width:342,height:640,page:0});
    requests.at(-1).success({data:friends});await Promise.resolve();await Promise.resolve();
    const latest=JSON.stringify(texts);stale.success({data:[]});await Promise.resolve();await Promise.resolve();
    assert.equal(JSON.stringify(texts),latest,'stale async response cannot overwrite resized board');
    console.log('unit-test PASS: leaderboard aspect, raster density, responsive rows/pages, unchanged-layout caching and stale response isolation');
}
main().catch(error=>{console.error(error);process.exitCode=1;});
