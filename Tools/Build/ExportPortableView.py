"""Export a shared-session project for the engine's rectangular 2D view hosts."""
import hashlib
import json
import shutil
from pathlib import Path


def export(project, output):
    project, output = Path(project).resolve(), Path(output).resolve()
    config = json.loads((project / 'Config/Portable.json').read_text(encoding='utf-8-sig'))
    descriptor = json.loads((project / 'Project.heproject').read_text(encoding='utf-8-sig'))
    if config.get('schema') != 1: raise ValueError('Unsupported portable content schema')
    output.mkdir(parents=True, exist_ok=True)
    assets = {}
    for name in config['assets']:
        source = (project / name).resolve()
        if not source.is_relative_to(project) or not name.startswith('Assets/'): raise ValueError('Asset escapes project')
        target = output / name
        target.parent.mkdir(parents=True, exist_ok=True)
        data = source.read_bytes(); target.write_bytes(data)
        assets[name] = dict(bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
    shutil.copyfile(project / 'Config/GameRules.json', output / 'catalog.json')
    (output / 'portable-game.json').write_text(json.dumps(dict(schema=1,projectId=descriptor['ProjectId'],title=descriptor['DisplayName']),ensure_ascii=False),encoding='utf-8')
    (output / 'content-manifest.json').write_text(json.dumps(dict(schema=1,assets=assets)),encoding='utf-8')
    (output / 'index.html').write_text('<!doctype html><html lang="zh"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>HEngine</title><style>html,body{margin:0;overflow:hidden;background:#122230}canvas{width:100vw;height:100vh;display:block;touch-action:none}</style><canvas></canvas><script type="module" src="./main.js"></script></html>',encoding='utf-8')
    (output / 'main.js').write_text("import {startPortableGame} from './engine/Portable/BrowserHost.js';\nstartPortableGame().catch(error=>{console.error(error);document.body.textContent='游戏启动失败：'+error.message;});\n",encoding='utf-8')
    licenses = output / 'Licenses'
    licenses.mkdir(exist_ok=True)
    engine = Path(__file__).resolve().parents[2]
    shutil.copyfile(engine / 'LICENSE', licenses / 'HEngine.txt')
    runtime = engine / 'Scripting/CSharp/Runtime'
    if not (runtime / 'LICENSE.txt').exists():
        dotnet = shutil.which('dotnet')
        if not dotnet: raise ValueError('Cannot locate .NET runtime notices')
        runtime = Path(dotnet).resolve().parent
    for name in ('LICENSE.txt', 'ThirdPartyNotices.txt'):
        shutil.copyfile(runtime / name, licenses / ('DotNet-' + name))
    if (project / 'Legal').exists():
        for source in (project / 'Legal').rglob('*'):
            if not source.resolve().is_relative_to(project): raise ValueError('Legal file escapes project')
            if source.is_file():
                target = licenses / 'Project' / source.relative_to(project / 'Legal')
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source, target)
    print(f'Portable view content PASS: {len(assets)} assets')
