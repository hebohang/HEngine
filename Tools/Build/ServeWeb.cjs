'use strict';
const fs=require('node:fs'),path=require('node:path'),http=require('node:http');
const root=fs.realpathSync(path.resolve(process.argv[2]));
const port=Number(process.argv[3]||8769);
const mime={'.html':'text/html; charset=utf-8','.js':'text/javascript; charset=utf-8','.css':'text/css; charset=utf-8','.wasm':'application/wasm','.json':'application/json','.png':'image/png','.jpg':'image/jpeg'};
const server=http.createServer((req,res)=>{try{
    if(req.method!=='GET'&&req.method!=='HEAD'){res.writeHead(405).end();return;}
    const name=decodeURIComponent(new URL(req.url,'http://localhost').pathname).replace(/\/$/,'/index.html');
    const file=fs.realpathSync(path.resolve(root,'.'+name));if(!file.startsWith(root+path.sep)){res.writeHead(403).end();return;}
    const stat=fs.statSync(file);if(!stat.isFile()){res.writeHead(404).end();return;}
    res.writeHead(200,{'Content-Type':mime[path.extname(file)]||'application/octet-stream','Content-Length':stat.size,'Cache-Control':'no-cache','X-Content-Type-Options':'nosniff'});
    if(req.method==='HEAD')res.end();else fs.createReadStream(file).on('error',()=>res.destroy()).pipe(res);
}catch{res.writeHead(404).end();}});
server.listen(port,'127.0.0.1',()=>{const url=`http://127.0.0.1:${server.address().port}/`;console.log('HEngine Web: '+url);if(process.argv.includes('--open'))require('node:child_process').spawn('explorer.exe',[url],{windowsHide:true,stdio:'ignore'}).on('error',console.error);});
server.on('error',e=>{console.error(e.message);process.exitCode=1;});
for(const signal of ['SIGINT','SIGTERM'])process.on(signal,()=>server.close());
