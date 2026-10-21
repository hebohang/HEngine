'use strict';
const path=require('node:path'),fs=require('node:fs');
const root=path.resolve(__dirname,'..'),engine=process.env.HE_ENGINE_ROOT;
if(!engine)throw Error('HE_ENGINE_ROOT is required. Export through hectl publish.');
const {exportWeChatGame}=require(path.join(engine,'Tools/Build/ExportWeChatGame.cjs'));
const config=JSON.parse(fs.readFileSync(path.join(root,'Config/WeChat.json'),'utf8'));
exportWeChatGame({input:process.argv[2],managed:process.argv[3],output:process.argv[4],entryPoint:path.join(root,'WeChat/game.js'),config,contentFiles:['catalog.json','portable-game.json']})
    .then(result=>console.log(JSON.stringify(result))).catch(error=>{console.error(error);process.exitCode=1;});
