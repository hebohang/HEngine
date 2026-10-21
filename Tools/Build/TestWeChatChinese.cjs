'use strict';
// Audit visible text using the real exported catalog, including every menu page.
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),vm=require('node:vm');
const esbuild=require('../Scripting/node_modules/esbuild');
const {engineAliases}=require('./EngineImports.cjs');
const root=path.resolve(__dirname,'../..'),pkg=path.resolve(process.argv[2]||path.join(root,'Project/Dist/WeChat'));
const config=require(path.join(pkg,'he-game-config.js'));
const catalog=JSON.parse(fs.readFileSync(path.join(pkg,config.files['catalog.json'])));
const scope={};
for(const [name,file] of [['Hud','Project/WeChat/Hud.js'],['Hui','Engine/Source/Runtime/Platform/Web/HuiDocument.js']])
    vm.runInNewContext(esbuild.buildSync({entryPoints:[path.join(root,file)],bundle:true,write:false,format:'iife',globalName:name,alias:engineAliases()}).outputFiles[0].text,scope);
const seen=new Set();let draws=[];
const ctx={save(){},restore(){},scale(){},rect(){},clip(){},beginPath(){},moveTo(){},arcTo(){},arc(){},fill(){},stroke(){},drawImage(){},fillText(value){draws.push(String(value));}};
const snapshot={mode:'menu',profile:catalog.defaultProfile,character:catalog.characters[0].id,choices:[],health:100,maxHealth:100,level:1,kills:0,elapsed:0,regions:[],message:'本次冒险结束 · 获得 0 金币'};
const options={snapshot,catalog,images:new Map(),stick:{id:null,x:0,y:0},tab:'characters',page:0};
function check(){for(const text of draws){assert.ok(!/[a-z]/i.test(text),'English leaked into UI: '+text);seen.add(text);}draws=[];}
for(const group of ['characters','weapons','tomes','trinkets','power_ups','quests','difficulties','challenges','enemies','bosses'])
    for(const item of catalog[group]){
        const label=item.name?.zh_cn||item.title?.zh_cn;
        assert.ok(label&&/[\u3400-\u9fff]/.test(label)&&!/[a-z]/i.test(label),group+': '+item.id+' needs a Chinese label');
    }
for(const width of [390,960,1280]){
    snapshot.mode='menu';
    for(const tab of ['characters','weapons','powers','quests','settings']){
        options.tab=tab;
        for(let page=0;page<300;page++){
            options.page=page;const result=scope.Hud.paintHud(ctx,width,540,options);check();
            if(result.page<page)break;
            assert.ok(page<299,'pagination did not terminate');
        }
    }
    for(const mode of ['playing','upgrade','revive','result']){
        snapshot.mode=mode;snapshot.choices=catalog.weapons.slice(0,3).map(item=>({id:item.id}));
        scope.Hud.paintHud(ctx,width,540,options);check();
    }
    // A missing catalog entry must not expose an internal identifier.
    snapshot.mode='upgrade';snapshot.choices=[{id:'missing_english_identifier'}];
    scope.Hud.paintHud(ctx,width,540,options);check();
    const pause=JSON.parse(fs.readFileSync(path.join(pkg,config.files[catalog.sharedUi.pause])));
    new scope.Hui.HuiDocument(pause).paint(ctx,width,540);check();
}
for(const quest of catalog.quests)assert.ok(seen.has(quest.title.zh_cn),'Quest title was not rendered: '+quest.id);
assert.ok([...seen].some(text=>text.startsWith('等级 ')));
console.log(`unit-test PASS: ${catalog.quests.length} Chinese quest titles; all menu pages, gameplay, upgrade, revive, result, pause and missing-entry fallback at three widths (${seen.size} visible strings).`);
