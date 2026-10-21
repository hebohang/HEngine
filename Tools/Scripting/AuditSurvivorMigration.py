"""Inventory the real game's reachable Python modules and native API dependencies."""
import ast
import collections
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPTS = ROOT / "Project/Scripts"


def qualified(node):
    if isinstance(node, ast.Name):
        return node.id
    if isinstance(node, ast.Attribute):
        base = qualified(node.value)
        return base + "." + node.attr if base else ""
    return ""


def main():
    pending = ["SurvivorGame"]
    modules = {}
    while pending:
        name = pending.pop()
        if name in modules:
            continue
        path = SCRIPTS / (name.replace(".", "/") + ".py")
        if not path.is_file():
            path = SCRIPTS / name.replace(".", "/") / "__init__.py"
        if not path.is_file():
            continue
        data = path.read_bytes()
        source = data.decode("utf-8-sig")
        tree = ast.parse(source)
        imports = set()
        api = collections.Counter()
        functions = []
        for node in ast.walk(tree):
            if isinstance(node, ast.Import):
                imports.update(alias.name for alias in node.names)
            elif isinstance(node, ast.ImportFrom) and node.module:
                imports.add(node.module)
            elif isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
                functions.append(dict(name=node.name, line=node.lineno))
            elif isinstance(node, ast.Call):
                target = qualified(node.func)
                if target.startswith("HEngine."):
                    api[target] += 1
        modules[name] = dict(path=path.relative_to(ROOT).as_posix(), sha256=hashlib.sha256(data).hexdigest(),
                             lines=len(source.splitlines()), imports=sorted(imports), functions=functions,
                             directEngineCalls=dict(sorted(api.items())))
        pending.extend(imports - modules.keys())
    scenes = {}
    for name in ("MainMenu", "MainGame"):
        path = ROOT / f"Project/Assets/Scenes/{name}.he"
        entities = json.loads(path.read_text(encoding="utf-8-sig"))
        counts = collections.Counter(component for entity in entities.values() for component in entity)
        scenes[name] = dict(entities=len(entities), components=dict(sorted(counts.items())))
    report = dict(schema=1, entry="SurvivorGame", modules=dict(sorted(modules.items())), scenes=scenes,
                  limits=["Static direct-call inventory excludes dynamically named APIs and calls through component objects.",
                          "Reachable Python modules are not equivalent to completed C# ports.",
                          "Existing Web exporter targets ScriptTouch.he, not MainGame.he."])
    output = ROOT / "out/agent/survivor-migration/inventory.json"
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(dict(modules=len(modules), lines=sum(module["lines"] for module in modules.values()),
                          uniqueDirectEngineCalls=len({api for module in modules.values() for api in module["directEngineCalls"]}),
                          scenes=scenes, report=str(output)), ensure_ascii=False))


if __name__ == "__main__":
    main()
