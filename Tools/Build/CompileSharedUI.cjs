'use strict';
// Generate native RmlUi and validate portable UI from the same Editor-authored UIRes.
const fs=require('node:fs'),path=require('node:path'),vm=require('node:vm');
const esbuild=require('../Scripting/node_modules/esbuild');
const scope={};
vm.runInNewContext(esbuild.buildSync({entryPoints:[path.resolve(__dirname,'../../Engine/Source/Runtime/Platform/Web/HuiDocument.js')],bundle:true,write:false,format:'iife',globalName:'Hui'}).outputFiles[0].text,scope);
const {normalizeHui}=scope.Hui;
const escape=value=>String(value).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
function projectFile(project,name){
    if(typeof name!=='string'||!name.startsWith('Assets/')||name.includes('\\'))throw Error('Shared UI requires an Assets/ path: '+name);
    const file=path.resolve(project,name),real=fs.realpathSync(file),root=fs.realpathSync(project)+path.sep;
    if(!real.startsWith(root))throw Error('Shared UI path escapes project: '+name);
    return file;
}
function rml(document){
    function node(n){
        const l=n.Layout,s=n.Style,css=['position:absolute','box-sizing:border-box','overflow:hidden','margin:0dp','padding:0dp',`background-color:${s.BackgroundColor}`,`color:${s.TextColor}`,`font-size:${s.FontSize}dp`,`border-radius:${s.BorderRadius}dp`,`border:${s.BorderWidth}dp ${s.BorderColor}`,`pointer-events:${n.bInteractable?'auto':'none'}`];
        if(n.ParentId<0)css.push('left:0dp','top:0dp','width:100%','height:100%');
        else for(let axis=0;axis<2;axis++){
            const near=axis?'top':'left',far=axis?'bottom':'right',size=axis?'height':'width';
            css.push(`${near}:${l.AnchorMin[axis]*100}%`);
            if(l.AnchorMin[axis]!==l.AnchorMax[axis])css.push(`${far}:${(1-l.AnchorMax[axis])*100}%`,`margin-${near}:${l.OffsetMin[axis]}dp`,`margin-${far}:${-l.OffsetMax[axis]}dp`,`${size}:auto`);
            else css.push(`margin-${near}:${l.OffsetMin[axis]-l.Pivot[axis]*Math.max(0,l.OffsetMax[axis])}dp`,`${size}:${Math.max(0,l.OffsetMax[axis])}dp`);
        }
        if(!n.bVisible)css.push('display:none');
        if(s.BackgroundImage)css.push(`background-image:url('${s.BackgroundImage}')`);
        const font=s.FontPath.toLowerCase();
        css.push(`font-family:'${!font||font.includes('noto')?'Noto Sans SC':'Open Sans'}'`,'white-space:nowrap');
        const tag={Canvas:'div',Panel:'div',Text:'p',Image:'img',Button:'button'}[n.Type];
        const attrs=`id="${escape(n.ElementId)}" style="${escape(css.join(';'))}"`;
        const children=document.Nodes.filter(c=>c.ParentId===n.Id).map(node).join('');
        return `<${tag} ${attrs}${n.ImagePath?` src="${escape(n.ImagePath)}"`:''}>${escape(n.Text)}${children}</${tag}>`;
    }
    return `<rml><head><title>${escape(document.Name)}</title></head><body style="margin:0px;padding:0px;width:100%;height:100%;overflow:hidden;">${node(document.Nodes.find(n=>n.ParentId<0))}</body></rml>\n`;
}
function compile(project,{check=false}={}){
    project=path.resolve(project);
    const config=path.join(project,'Config/SharedUI.json');
    if(!fs.existsSync(config))return [];
    const settings=JSON.parse(fs.readFileSync(config,'utf8'));
    if(settings.schema!==1||!settings.documents||Array.isArray(settings.documents))throw Error('Invalid SharedUI schema');
    const result=[];
    // Validate the complete set before generating anything.
    for(const name of new Set(Object.values(settings.documents))){
        if(!name.endsWith('.hui'))throw Error('Shared UI must reference .hui: '+name);
        const file=projectFile(project,name),document=normalizeHui(JSON.parse(fs.readFileSync(file,'utf8')),name);
        for(const n of document.Nodes)for(const asset of [n.ImagePath,n.Style.BackgroundImage,n.Style.FontPath].filter(Boolean))projectFile(project,asset);
        for(const [field,extension] of [['GeneratedRmlPath','.rml'],['GeneratedRcssPath','.rcss']]){
            if(document[field]!==name.replace(/\.hui$/,extension))throw Error(`${name}: ${field} must point to its sibling`);
        }
        result.push({name,file,document});
    }
    if(!check)for(const item of result){
        for(const [extension,content] of [['.rml',rml(item.document)],['.rcss','/* Generated from UIRes by CompileSharedUI. */\n']]){
            const target=item.file.replace(/\.hui$/,extension);
            if(!fs.existsSync(target)||fs.readFileSync(target,'utf8')!==content)fs.writeFileSync(target,content);
        }
    }
    return result;
}
module.exports={compile,rml,projectFile};
if(require.main===module){try{const documents=compile(process.argv[2],{check:process.argv.includes('--check')});console.log(`Shared UI ${process.argv.includes('--check')?'validation':'generation'} PASS: ${documents.length} documents`);}catch(error){console.error(error.message);process.exitCode=1;}}
