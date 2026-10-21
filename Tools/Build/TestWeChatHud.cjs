'use strict';
const assert=require('node:assert/strict'),vm=require('node:vm'),path=require('node:path');
const esbuild=require('../Scripting/node_modules/esbuild');
const {engineAliases}=require('./EngineImports.cjs');
const code=esbuild.buildSync({entryPoints:[path.resolve(__dirname,'../../Project/WeChat/Hud.js')],bundle:true,write:false,format:'iife',globalName:'Hud',alias:engineAliases()}).outputFiles[0].text;
const scope={};vm.runInNewContext(code,scope);
for(const width of [390,1040,1280]){
    const height=540,backdrop={x:-72,y:-64,width:width+72,height:height+74},draws=[];
    const ctx={save(){},restore(){},rect(){},clip(){},beginPath(){},moveTo(){},arcTo(){},fill(){},fillText(){},drawImage:(...args)=>draws.push(args)};
    const items=Array.from({length:10},(_,i)=>({id:String(i),name:{zh_cn:'角色 '+i},icon:'icon'+i}));
    const state={mode:'menu',profile:{walletGold:0,owned:{characters:[]}},choices:items.slice(0,3),health:100,maxHealth:100,level:1,kills:0,elapsed:0,regions:[]};
    const options={snapshot:state,catalog:{characters:items,weapons:items,tomes:[]},images:new Map([['Assets/UI/Backgrounds/starbloom_title_bg.png',{width:1920,height:1080}]]),stick:{},tab:'characters',page:0,backdrop};
    const menu=scope.Hud.paintHud(ctx,width,height,options),[,x,y,w,h]=draws[0];
    assert.ok(x<=backdrop.x&&y<=backdrop.y&&x+w>=backdrop.x+backdrop.width&&y+h>=backdrop.y+backdrop.height,'background covers unsafe edges too');
    const tabs=menu.buttons.filter(b=>b.action==='tab');assert.ok(tabs[0].x<width*.1&&tabs.at(-1).x+tabs.at(-1).w>width*.9,'navigation spans viewport');
    for(const item of items)options.images.set(item.icon,{width:64,height:64,id:item.id});
    draws.length=0;state.mode='upgrade';const upgrade=scope.Hud.paintHud(ctx,width,height,options),choices=upgrade.buttons.filter(b=>b.action==='upgrade');
    assert.equal(choices.length,3);assert.ok(choices.at(-1).x+choices.at(-1).w-choices[0].x>width*.75,'upgrade choices span viewport');
    const hit=(x,y)=>upgrade.buttons.find(b=>x>=b.x&&x<=b.x+b.w&&y>=b.y&&y<=b.y+b.h);
    for(const [image,x,y,w,h] of draws)assert.equal(hit(x+w/2,y+h/2)?.value,image.id,'icon selects its upgrade');
    for(const b of choices){assert.equal(hit(b.x+2,b.y+2)?.value,b.value,'white card margin selects upgrade');assert.equal(hit(b.x+b.w/2,b.y+b.h-30)?.value,b.value,'bottom button selects same upgrade');}
    for(const b of [...menu.buttons,...upgrade.buttons])assert.ok(b.w>0&&b.h>0&&b.x>=0&&b.y>=0&&b.x+b.w<=width&&b.y+b.h<=height,'touch targets remain inside safe viewport');
}
console.log('unit-test PASS: full-bleed background, full-width navigation/upgrade choices, safe touch bounds at narrow/landscape/ultrawide sizes');
