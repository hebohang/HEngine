'use strict';
// Runs the public exporter/WeChat loader with real Wasm, with EH probes rejected.
const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
const {spawn,spawnSync}=require('node:child_process');
const {exportManaged}=require('./ExportManagedWeChat.cjs');
const {validateManagedWeChat}=require('./ManagedWeChatCompatibility.cjs');
const esbuild=require('../Scripting/node_modules/esbuild');
const {chromium}=require('playwright');
async function main(){
    const input=path.resolve(process.argv[2]),output=path.resolve(__dirname,'../../out/agent/wechat-compatibility-test',Date.now().toString());
    fs.mkdirSync(output,{recursive:true});
    const manifest={resources:{jsModuleNative:[{name:'native.js'}]}};
    assert.throws(()=>validateManagedWeChat(manifest,()=>Buffer.from('DOTNET.setup({wasmEnableEH:true,wasmEnableSIMD:false,other:0})')),/WasmEnableExceptionHandling=false/);
    assert.throws(()=>validateManagedWeChat(manifest,()=>Buffer.from('DOTNET.setup({wasmEnableEH:false,wasmEnableSIMD:true,other:0})')),/WasmEnableSIMD=false/);
    assert.throws(()=>validateManagedWeChat(manifest,()=>Buffer.from('unknown SDK')),/Unknown/);
    const config=exportManaged(input,output);
    const wasm=path.join(output,'runtime',config.resources.wasmNative[0].name);
    // Disable the VM's actual native EH support: no capability spoofing.
    const binary=spawnSync(process.execPath,['--no-experimental-wasm-legacy-eh','--no-experimental-wasm-exnref','-e',
        'const fs=require("fs"),path=require("path");new WebAssembly.Module(fs.readFileSync(process.argv[1]));const dir=path.dirname(process.argv[1]);if(!fs.readdirSync(dir).filter(n=>/^feature-.*wasm$/.test(n)).some(n=>!WebAssembly.validate(fs.readFileSync(path.join(dir,n)))))throw Error("Negative feature control did not fail");',wasm],{encoding:'utf8',windowsHide:true});
    assert.equal(binary.status,0,binary.stderr);
    if(process.argv[3]){
        const gameRoot=path.resolve(process.argv[3]),gameConfig=require(path.join(gameRoot,'runtime-config.js'));
        const gameBinary=spawnSync(process.execPath,['--no-experimental-wasm-legacy-eh','--no-experimental-wasm-exnref','-e',
            'new WebAssembly.Module(require("fs").readFileSync(process.argv[1]));',path.join(gameRoot,'runtime',gameConfig.resources.wasmNative[0].name)],{encoding:'utf8',windowsHide:true});
        assert.equal(gameBinary.status,0,gameBinary.stderr);
    }
    const entry=`const {createManagedRuntime}=require('./wechat-runtime.js');const config=require('./runtime-config.js');
window.runCompatibilityTest=async()=>{const file='runtime/'+config.resources.wasmNative[0].name;window.packageModules.set(file,await (await fetch(file)).arrayBuffer());const runtime=await createManagedRuntime(config);const api=(await runtime.getAssemblyExports(config.mainAssemblyName)).Program;
const result=api.ExceptionRoundTrip();let propagated=false;try{api.ThrowToHost()}catch(e){propagated=String(e).includes('expected-host-exception')}
return {result,propagated,afterException:api.ExceptionRoundTrip(),rejectedProbes:window.rejectedProbes,pathModules:window.pathModules};};`;
    fs.writeFileSync(path.join(output,'entry.cjs'),entry);
    esbuild.buildSync({entryPoints:[path.join(output,'entry.cjs')],bundle:true,platform:'browser',outfile:path.join(output,'test.js')});
    fs.writeFileSync(path.join(output,'index.html'),'<script src="test.js"></script>');
    const server=spawn(process.execPath,[path.join(__dirname,'ServeWeb.cjs'),output,'0'],{windowsHide:true,stdio:['ignore','pipe','pipe']});
    let browser;
    try{
        const url=await new Promise((resolve,reject)=>{server.stdout.on('data',b=>{const m=String(b).match(/http:\/\/127\.0\.0\.1:\d+\//);if(m)resolve(m[0]);});server.on('error',reject);server.on('exit',c=>reject(Error('Server exit '+c)));});
        browser=await chromium.launch({executablePath:process.env.HE_SCRIPT_CHROME||'C:/Program Files/Google/Chrome/Application/chrome.exe',headless:true,args:['--js-flags=--no-experimental-wasm-legacy-eh --no-experimental-wasm-exnref']});
        const page=await browser.newPage(),errors=[];
        page.on('pageerror',e=>errors.push(String(e)));
        page.on('console',m=>{if(m.type()==='error')console.error(m.text());});
        await page.addInitScript(()=>{
            // DevTools supplies browser globals that are absent on phones.
            for(const name of ['performance','TextEncoder','TextDecoder','crypto'])
                Object.defineProperty(window,name,{value:undefined,configurable:true});
            window.rejectedProbes=0;
            window.packageModules=new Map();window.pathModules=0;
            const checkPath=file=>{if(typeof file!=='string'||!/^runtime\/[a-zA-Z0-9_.-]+\.wasm(?:\.br)?$/.test(file))throw Error('WXWebAssembly.Module: only support file type .wasm or .wasm.br');};
            const bytes=async file=>{const response=await fetch(file);if(!response.ok)throw Error('Missing package file '+file);return response.arrayBuffer();};
            window.wx={getFileSystemManager:()=>({readFile:({filePath,success,fail})=>bytes(filePath).then(data=>success({data})).catch(fail)})};
            window.WXWebAssembly={Memory:WebAssembly.Memory,Table:WebAssembly.Table,Global:WebAssembly.Global,
                Module:function(file){checkPath(file);window.pathModules++;if(!window.packageModules.has(file))throw Error('Missing module '+file);return new WebAssembly.Module(window.packageModules.get(file));},
                instantiate:async(file,imports)=>{checkPath(file);if(file.includes('/feature-')){window.rejectedProbes++;throw Error('Simulated mobile: optional Wasm feature unavailable');}return WebAssembly.instantiate(await bytes(file),imports);}};
        });
        await page.goto(url);
        const result=await page.evaluate(()=>Promise.race([window.runCompatibilityTest(),new Promise((_,reject)=>setTimeout(()=>reject(Error('Runtime startup timed out')),30000))]));
        assert.equal(result.result,42);assert.equal(result.afterException,42);assert.equal(result.propagated,true);assert.equal(result.rejectedProbes,3);assert.deepEqual(errors,[]);
        assert.ok(result.pathModules>0,'Real runtime must use the strict package-path Module constructor');
        fs.writeFileSync(path.join(output,'result.json'),JSON.stringify({status:'PASS',evidenceType:'runtime-manual-browser-harness',...result,missingGlobals:['performance','TextEncoder','TextDecoder','crypto','WXWebAssembly.compile'],binaryWithoutNativeEH:'PASS',negativeFeatureControl:'PASS',gameBinaryWithoutNativeEH:process.argv[3]?'PASS':'NOT RUN',incompatibleExportRejected:'PASS'},null,2));
        console.log('PASS: independent C# exceptions, finally, JSON exceptions, host propagation and recovery; all optional feature probes unavailable; real binary compiles with native EH disabled. Evidence: '+output);
    }finally{if(browser)await browser.close();server.kill();}
}
main().catch(e=>{console.error(e);process.exitCode=1;});
