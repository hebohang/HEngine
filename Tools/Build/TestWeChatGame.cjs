'use strict';
// Exercise the shared packager with a separate project, no Survivor imports.
const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict'),crypto=require('node:crypto');
const {exportWeChatGame}=require('./ExportWeChatGame.cjs');
async function main(){
    const managed=path.resolve(process.argv[2]);
    const root=path.resolve(__dirname,'../../out/agent/wechat-independent-project',String(Date.now()));
    const input=path.join(root,'content'),output=path.join(root,'package'),entryPoint=path.join(root,'main.js');
    fs.mkdirSync(input,{recursive:true});
    const bytes=Buffer.from('{"independent":true}'),hash=b=>crypto.createHash('sha256').update(b).digest('hex');
    fs.writeFileSync(path.join(input,'level.json'),bytes);
    fs.writeFileSync(path.join(input,'content-manifest.json'),JSON.stringify({schema:1,assets:{'level.json':{bytes:bytes.length,sha256:hash(bytes)}}}));
    fs.writeFileSync(entryPoint,"const config=require('./he-game-config.js');module.exports=config.files;");
    await exportWeChatGame({input,managed,output,entryPoint,config:{appid:'touristappid',projectName:'Independent Fixture',orientation:'landscape'}});
    const config=require(path.join(output,'he-game-config.js'));
    assert.deepEqual(fs.readFileSync(path.join(output,config.files['level.json'])),bytes);
    assert.equal(JSON.parse(fs.readFileSync(path.join(output,'project.config.json'))).projectname,'Independent Fixture');
    assert.equal(JSON.parse(fs.readFileSync(path.join(output,'game.json'))).deviceOrientation,'landscape');
    assert.equal(config.files['catalog.json'],undefined);
    assert.equal(require(path.join(output,'game.js'))['level.json'],config.files['level.json']);
    await assert.rejects(exportWeChatGame({input,managed,output,entryPoint,config:{}}),/empty staging/);
    console.log('unit-test PASS: independent project content, entry, name, orientation and nonempty output protection. '+output);
}
main().catch(e=>{console.error(e);process.exitCode=1;});
