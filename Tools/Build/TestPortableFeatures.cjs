'use strict';
const assert=require('node:assert/strict'),path=require('node:path'),fs=require('node:fs'),vm=require('node:vm');
const esbuild=require('../Scripting/node_modules/esbuild');
const root=path.resolve(__dirname,'../..'),out=path.join(root,'out/agent/portable-feature-tests');fs.mkdirSync(out,{recursive:true});
function load(name){const file=path.join(out,path.basename(name)+'.cjs');esbuild.buildSync({entryPoints:[path.join(root,'Engine/Source/Runtime/Platform',name)],outfile:file,bundle:true,platform:'node',format:'cjs'});return require(file);}
async function main(){
    const {viewportLayout}=load('Web/ViewportLayout.js'),{InterestRegion}=load('Web/InterestRegion.js'),{Advertising}=load('WeChat/Advertising.js');
    const {applyOrientation}=load('WeChat/Viewport.js');let direction='';
    await applyOrientation({setDeviceOrientation:({value,complete})=>{direction=value;complete();}},'landscape');assert.equal(direction,'landscape');
    for(const [width,height]of [[390,844],[844,390],[1920,1080],[320,568]]){
        const layout=viewportLayout({width,height,left:20,right:20,top:40,bottom:10});
        const point=layout.point(layout.left+78*layout.scale,layout.top+(layout.height-90)*layout.scale);
        assert.ok(Math.abs(point.x-78)<1e-6);assert.ok(Math.abs(point.y-(layout.height-90))<1e-6);
        assert.ok(layout.height>=540);assert.equal(layout.landscape,width>height);
    }
    const region=new InterestRegion({enabled:true,radius:55,padding:25,animationHz:20});
    assert.equal(region.contains(60,0,0,0),false);assert.equal(region.contains(60,0,0,0,region.padding),true);
    assert.equal(region.animationTime(.021),region.animationTime(.041));
    assert.equal(new InterestRegion().contains(99999,0,0,0),true);
    const listeners={};let shows=0;
    const ad={show:async()=>{shows++;},load:async()=>{},onClose:fn=>listeners.close=fn,offClose:()=>delete listeners.close,onError:fn=>listeners.error=fn,offError:()=>delete listeners.error};
    const service=new Advertising({createRewardedVideoAd:()=>ad,createInterstitialAd:()=>ad},{rewardedId:'fixture',interstitialId:'fixture'});
    const ready=()=>new Promise(resolve=>setImmediate(resolve));
    let request=service.showRewarded();await ready();assert.equal((await service.showRewarded()).status,'busy');listeners.close({isEnded:false});assert.equal((await request).completed,false);
    request=service.showRewarded();await ready();listeners.close({isEnded:true});assert.equal((await request).completed,true);assert.equal(service.busy,false);
    request=service.showRewarded();await ready();listeners.error({errMsg:'no fill'});assert.equal((await request).completed,false);
    request=service.showInterstitial();await ready();listeners.close();assert.equal((await request).completed,false);
    assert.equal((await new Advertising({}).showRewarded()).status,'unavailable');assert.equal(shows,4);
    // Execute the actual open-data script with isolated cloud storage and canvas.
    const texts=[],store=new Map();let onMessage;
    const ctx={clearRect(){},fillRect(){},fillText(text){texts.push(text);}};
    const wx={getSharedCanvas:()=>Object.freeze({width:720,height:640,getContext:()=>ctx}),onMessage:fn=>onMessage=fn,
        getUserCloudStorage:({keyList,success})=>success({KVDataList:keyList.map(key=>({key,value:store.get(key)||'0'}))}),
        setUserCloudStorage:({KVDataList,success})=>{KVDataList.forEach(v=>store.set(v.key,v.value));success({});},
        getFriendCloudStorage:({success})=>success({data:[{nickname:'A',KVDataList:[{key:'score',value:'5'}]},{nickname:'B',KVDataList:[{key:'score',value:'10'}]}]})};
    vm.runInNewContext(fs.readFileSync(path.join(root,'Engine/Source/Runtime/Platform/WeChat/OpenDataLeaderboard.js'),'utf8'),{wx,console});
    onMessage({type:'he-leaderboard-score',key:'score',score:100});onMessage({type:'he-leaderboard-score',key:'score',score:10});await ready();assert.equal(store.get('score'),'100');
    onMessage({type:'he-leaderboard-show',key:'score',page:0});await ready();assert.ok(texts.includes('1. B'));assert.ok(texts.includes('2. A'));
    console.log('unit-test PASS: landscape/portrait safe-area hit tests, ROI/prefetch/animation cadence, ad completion/cancel/error/concurrency, open-data ranking and monotonic cloud score');
}
main().catch(error=>{console.error(error);process.exitCode=1;});
