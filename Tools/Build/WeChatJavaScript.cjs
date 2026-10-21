'use strict';
const esbuild=require('../Scripting/node_modules/esbuild');
function compileWeChatJavaScript(source,name){
    const result=esbuild.transformSync(String(source),{target:'es2019',sourcefile:name});
    if(result.warnings.length)throw Error('WeChat JavaScript compatibility: '+name+': '+result.warnings.map(w=>w.text).join('; '));
    return result.code;
}
function validateWeChatJavaScript(source,name){
    const normalized=esbuild.transformSync(String(source),{target:'esnext',sourcefile:name}).code;
    if(compileWeChatJavaScript(source,name)!==normalized)
        throw Error('WeChat upload syntax incompatible: '+name+'. Run every JavaScript file through the engine ES2019 export transform, including copied host helpers.');
}
module.exports={compileWeChatJavaScript,validateWeChatJavaScript};
if(require.main===module){
    const fs=require('node:fs'),path=require('node:path'),root=path.resolve(process.argv[2]);let count=0;
    function visit(dir){for(const entry of fs.readdirSync(dir,{withFileTypes:true})){
        const file=path.join(dir,entry.name);
        if(entry.isDirectory())visit(file);
        else if(entry.name.endsWith('.js')){validateWeChatJavaScript(fs.readFileSync(file,'utf8'),path.relative(root,file));count++;}
    }}
    visit(root);console.log('PASS: WeChat ES2019 upload syntax, '+count+' JavaScript files');
}
