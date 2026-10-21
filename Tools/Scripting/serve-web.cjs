'use strict';
// Local development server for the verified export; no runtime or package dependency.
const fs = require('node:fs'), path = require('node:path'), http = require('node:http');
const root = fs.realpathSync(path.resolve(process.argv[2] || 'out/agent/scripting/game-web'));
const port = Number(process.argv[3] || 8768);
if (!Number.isInteger(port) || port < 1 || port > 65535) throw new Error('Invalid HTTP port.');
const types = {'.wasm':'application/wasm','.js':'text/javascript; charset=utf-8','.json':'application/json; charset=utf-8',
    '.html':'text/html; charset=utf-8','.png':'image/png','.wav':'audio/wav','.txt':'text/plain; charset=utf-8'};
const server = http.createServer((request,response) => {
    try {
        if (!['GET','HEAD'].includes(request.method)) { response.writeHead(405,{Allow:'GET, HEAD'}).end(); return; }
        let name = decodeURIComponent(new URL(request.url,'http://localhost').pathname).slice(1) || 'index.html';
        const manifest = JSON.parse(fs.readFileSync(path.join(root,'game-manifest.json'),'utf8'));
        if (name !== 'game-manifest.json' && !manifest.assets.some(asset => asset.path === name)) { response.writeHead(404).end(); return; }
        const file = fs.realpathSync(path.resolve(root,name));
        if (!file.startsWith(root + path.sep) || !fs.statSync(file).isFile()) { response.writeHead(404).end(); return; }
        response.writeHead(200,{'Content-Type':types[path.extname(file)] || 'application/octet-stream',
            'Content-Length':fs.statSync(file).size,'Cache-Control':'no-cache','X-Content-Type-Options':'nosniff'});
        if (request.method === 'HEAD') response.end();
        else fs.createReadStream(file).on('error',() => response.destroy()).pipe(response);
    } catch { if (!response.headersSent) response.writeHead(404); response.end(); }
});
server.listen(port,'127.0.0.1',() => console.log(`HEngine Web: http://127.0.0.1:${port}/`));
server.on('error',error => {console.error(error.message);process.exitCode=1;});
for (const signal of ['SIGINT','SIGTERM']) process.on(signal,() => server.close());
