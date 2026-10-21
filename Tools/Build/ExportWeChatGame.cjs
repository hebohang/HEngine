'use strict';
const fs=require('node:fs'),path=require('node:path'),crypto=require('node:crypto');
const root=path.resolve(__dirname,'../..');
const {exportManaged}=require('./ExportManagedWeChat.cjs');
const esbuild=require('../Scripting/node_modules/esbuild');
const {engineAliases,engineSiteImports}=require('./EngineImports.cjs');
// Project callers supply their entry point, content list and platform settings.
async function exportWeChatGame({input,managed,output,entryPoint,config,contentFiles=[]}){
    input=path.resolve(input);output=path.resolve(output);
    if(fs.existsSync(output)&&fs.readdirSync(output).length)throw Error('WeChat exporter requires an empty staging directory');
    fs.mkdirSync(output,{recursive:true});
    exportManaged(path.resolve(managed),output);
    const files={},parts=['managed'],hash=b=>crypto.createHash('sha256').update(b).digest('hex');
    const manifest=JSON.parse(fs.readFileSync(path.join(input,'content-manifest.json')));
    let group=0,groupBytes=0;
    const names=[...new Set([...contentFiles,'content-manifest.json',...Object.keys(manifest.assets)])];
    for(const name of names){
        const source=path.resolve(input,name);if(!source.startsWith(input+path.sep))throw Error('Asset path escape');
        const bytes=fs.readFileSync(source);
        if(manifest.assets[name]&&hash(bytes)!==manifest.assets[name].sha256)throw Error('Modified staged asset: '+name);
        if(groupBytes+bytes.length>3*1024*1024){group++;groupBytes=0;}
        const directory='content'+group;
        if(!parts.includes(directory)){parts.push(directory);fs.mkdirSync(path.join(output,directory),{recursive:true});fs.writeFileSync(path.join(output,directory,'game.js'),'module.exports={};');}
        const ext=/\.(png|jpg|jpeg|wav|ogg|mp3)$/.test(name)?path.extname(name):'.bin';
        const packaged=directory+'/'+hash(name).slice(0,24)+ext;
        fs.writeFileSync(path.join(output,packaged),bytes);files[name]=packaged;groupBytes+=bytes.length;
    }
    fs.writeFileSync(path.join(output,'he-game-config.js'),'module.exports='+JSON.stringify({files,subpackages:parts,debug:config.debug===true,orientation:config.orientation||'portrait',ads:config.ads||{},leaderboard:config.leaderboard||{}})+';');
    fs.writeFileSync(path.join(output,'game.json'),JSON.stringify({deviceOrientation:config.orientation||'portrait',...(config.leaderboard?.enabled?{openDataContext:'open-data'}:{}),subpackages:parts.map(name=>({name,root:name==='managed'?'runtime/':name+'/'}))}));
    fs.writeFileSync(path.join(output,'project.config.json'),JSON.stringify({appid:config.appid,projectname:config.projectName||'HEngine Game',compileType:'game',libVersion:config.libVersion||'3.17.2',setting:{es6:false,minified:false,urlCheck:false}},null,2));
    if(fs.existsSync(path.join(input,'Licenses')))fs.cpSync(path.join(input,'Licenses'),path.join(output,'Licenses'),{recursive:true});
    await esbuild.build({entryPoints:[path.resolve(entryPoint)],outfile:path.join(output,'game.js'),bundle:true,format:'cjs',target:'es2019',alias:engineAliases(root),external:['./he-game-config.js','./wechat-runtime.js','./runtime-config.js'],plugins:[engineSiteImports(root)]});
    if(config.leaderboard?.enabled)await esbuild.build({entryPoints:[path.join(root,'Engine/Source/Runtime/Platform/WeChat/OpenDataLeaderboard.js')],outfile:path.join(output,'open-data/index.js'),bundle:true,format:'cjs',target:'es2019'});
    let bytes=0;const inventory=[];
    function visit(dir){for(const entry of fs.readdirSync(dir,{withFileTypes:true})){const file=path.join(dir,entry.name);if(entry.isDirectory())visit(file);else{const data=fs.readFileSync(file);bytes+=data.length;inventory.push({path:path.relative(output,file).replaceAll('\\','/'),bytes:data.length,sha256:hash(data)});}}}
    visit(output);
    fs.writeFileSync(path.join(output,'game-manifest.json'),JSON.stringify({schema:1,gameplay:'C#',bytes,assets:inventory},null,2));
    if(bytes>20*1024*1024)throw Error('WeChat export exceeds the configured 20 MiB package budget');
    return {output,bytes,mebibytes:bytes/1048576};
}
module.exports={exportWeChatGame};
