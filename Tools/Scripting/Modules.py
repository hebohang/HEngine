"""Versioned script module graph and content-addressed build inputs.

Only this build-time tool discovers files. Runtime registration stays generated.
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import xml.sax.saxutils as xml


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def encoded(value) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode("utf-8")


def report_status(module: dict, status: str) -> None:
    print("HE_SCRIPT_MODULE_STATUS=" + json.dumps({"id": module["id"], "assembly": module["assembly"], "status": status}), flush=True)


def report_diagnostics(module: dict, log: Path) -> None:
    pattern = re.compile(r"^(.+)\((\d+),(\d+)\):\s*(error|warning)\s+(\w+):\s*(.+?)(?:\s+\[[^\]]+\])?$")
    seen = set()
    for line in log.read_text(encoding="utf-8-sig", errors="replace").splitlines():
        match = pattern.match(line)
        if not match or match.groups() in seen:
            continue
        seen.add(match.groups())
        file, row, column, severity, code, message = match.groups()
        print("HE_SCRIPT_MODULE_DIAGNOSTIC=" + json.dumps({"file": file, "line": int(row), "column": int(column),
            "severity": severity, "code": code, "message": message, "module": module["id"], "log": str(log)}), flush=True)
        if len(seen) >= 100:
            break


def inside(root: Path, relative: str) -> Path:
    path = (root / relative).resolve()
    if Path(relative).is_absolute() or not path.is_relative_to(root.resolve()):
        raise ValueError(f"Module input escapes project: {relative}")
    return path


def graph(project: Path, platform: str, categories: set[str]) -> list[dict]:
    modules, names = {}, set()
    for path in sorted((project / "Scripts").rglob("*.hemodule")):
        value = json.loads(path.read_text(encoding="utf-8-sig"))
        allowed = {"schema", "id", "assembly", "sources", "references", "api", "includePlatforms", "excludePlatforms", "category", "defines", "unsafe", "generatedInputs"}
        if set(value) - allowed or value.get("schema") != 1:
            raise ValueError(f"Unsupported module definition: {path}")
        identity, name = value.get("id", ""), value.get("assembly", "")
        if not re.fullmatch(r"[A-Za-z0-9_.-]{1,128}", identity) or identity in modules:
            raise ValueError(f"Duplicate or invalid stable module ID: {identity}")
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_.]{0,127}", name) or name.lower() in names or name.startswith(("System.", "HEngine.")) or name == "GameScripts":
            raise ValueError(f"Duplicate or reserved assembly name: {name}")
        names.add(name.lower())
        if value.get("category", "Runtime") not in {"Runtime", "Editor", "Tests"}:
            raise ValueError(f"Invalid module category: {identity}")
        include = set(value.get("includePlatforms", ["Windows", "Web", "WeChat", "macOS"]))
        exclude = set(value.get("excludePlatforms", []))
        if (include | exclude) - {"Windows", "Web", "WeChat", "macOS"} or ("includePlatforms" in value and include & exclude):
            raise ValueError(f"Contradictory or unknown module platforms: {identity}")
        if set(value.get("api", ["HEngine.ScriptCore"])) != {"HEngine.ScriptCore"}:
            raise ValueError(f"Unsupported engine API reference: {identity}")
        if any(not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", item) for item in value.get("defines", [])) or not isinstance(value.get("unsafe", False), bool):
            raise ValueError(f"Invalid compiler settings: {identity}")
        value["definition"] = path.relative_to(project).as_posix()
        value["active"] = platform in include - exclude
        value["category"] = value.get("category", "Runtime")
        modules[identity] = value
    for identity, value in modules.items():
        references = value.get("references", [])
        if len(set(references)) != len(references):
            raise ValueError(f"Duplicate module reference: {identity}")
        for reference in references:
            if reference not in modules:
                raise ValueError(f"Missing module {reference}, referenced by {identity}")
            if value["category"] == "Runtime" and modules[reference]["category"] != "Runtime":
                raise ValueError(f"Runtime module {identity} leaks {modules[reference]['category']} dependency {reference}")
    result, visiting, visited = [], [], set()

    def visit(identity: str):
        if identity in visiting:
            raise ValueError("Module cycle: " + " -> ".join(visiting + [identity]))
        if identity in visited:
            return
        visiting.append(identity)
        value = modules[identity]
        for reference in sorted(value.get("references", [])):
            visit(reference)
        visiting.pop()
        visited.add(identity)
        result.append(value)
    for identity in sorted(modules):
        visit(identity)  # Diagnose cycles even in currently excluded modules.
    selected = [value for value in result if value["active"] and value["category"] in categories]
    selected_ids = {value["id"] for value in selected}
    for value in selected:
        for reference in value.get("references", []):
            if reference not in selected_ids:
                raise ValueError(f"Platform/category excludes dependency {reference} of {value['id']}")
    return selected


def inputs(project: Path, module: dict) -> list[dict]:
    files: set[Path] = set()
    for source in module.get("sources", []):
        path = inside(project, source)
        if path.is_dir():
            files.update(item.resolve() for item in path.rglob("*.cs") if not {"bin", "obj"} & set(item.relative_to(path).parts))
        elif path.is_file() and path.suffix == ".cs":
            files.add(path)
        else:
            raise ValueError(f"Missing C# module source: {source}")
    if not files:
        raise ValueError(f"Module {module['id']} has no C# inputs")
    for relative in module.get("generatedInputs", []):
        path = inside(project, relative)
        if not path.is_file():
            raise ValueError(f"Missing generated input: {relative}")
        files.add(path)
    files.add(inside(project, module["definition"]))
    result = []
    for path in sorted(files):
        if not path.is_relative_to(project):
            raise ValueError(f"Module source link escapes project: {path}")
        result.append({"path": path.relative_to(project).as_posix(), "sha256": digest(path.read_bytes())})
    return result


def plan(project: Path, platform: str, categories: set[str], configuration: str, sdk: str, api: str) -> list[dict]:
    result, identities, owners = [], {}, {}
    for module in graph(project, platform, categories):
        sources = inputs(project, module)
        for source in sources:
            if source["path"].endswith(".cs"):
                if source["path"] in owners:
                    raise ValueError(f"C# source belongs to two modules: {source['path']}")
                owners[source["path"]] = module["id"]
        key = digest(encoded({"module": module, "inputs": sources, "dependencies": {ref: identities[ref] for ref in module.get("references", [])},
            "platform": platform, "configuration": configuration, "sdk": sdk, "api": api}))
        identities[module["id"]] = key
        result.append({"module": module, "inputs": sources, "identity": key})
    return result


def build(project: Path, engine: Path, work: Path, platform: str, configuration: str, categories: set[str], dotnet: str) -> dict:
    sdk = subprocess.check_output([dotnet, "--version"], cwd=engine / "Engine/Managed", text=True).strip()
    if sdk != "10.0.302":
        raise ValueError(f"Module build requires SDK 10.0.302, found {sdk}")
    api_files = sorted((engine / "Engine/Managed/HEngine.ScriptCore").glob("*.cs")) + [engine / "Tools/Scripting/Api.schema.json", engine / "Tools/Scripting/GenerateApi.cmake"]
    api_files += sorted((engine / "Engine/Managed/HEngine.ScriptCore").glob("*.csproj"))
    api_files += sorted((engine / "Engine/Managed").glob("*.props"))
    api_files += sorted((engine / "Engine/Managed").glob("*.targets"))
    api_files += sorted((engine / "Tools/Scripting/RegistryGenerator").glob("*.cs")) + [Path(__file__).resolve()]
    api = digest(encoded([(path.name, digest(path.read_bytes())) for path in api_files]))
    planned = plan(project, platform, categories, configuration, sdk, api)
    outputs, results = {}, []
    for item in planned:
        module, key = item["module"], item["identity"]
        directory = work / key
        receipt_path = directory / "module-build.json"
        if receipt_path.exists():
            receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
            dll = directory / "output" / (module["assembly"] + ".dll")
            if receipt["identity"] == key and dll.is_file() and digest(dll.read_bytes()) == receipt["sha256"]:
                outputs[module["id"]] = dll
                results.append({**receipt, "cached": True, "dll": str(dll)})
                report_status(module, "cached")
                continue
        directory.mkdir(parents=True, exist_ok=True)
        escape = lambda value: xml.escape(str(value), {'"': '&quot;'})
        includes = "".join(f'<Compile Include="{escape(project / source["path"])}" />' for source in item["inputs"] if source["path"].endswith(".cs"))
        references = "".join(f'<Reference Include="{escape(reference)}"><HintPath>{escape(outputs[reference])}</HintPath></Reference>' for reference in module.get("references", []))
        defines = ";".join(module.get("defines", []) + ["HE_" + platform.upper()])
        csproj = directory / (module["assembly"] + ".csproj")
        csproj.write_text(f'''<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup>
<TargetFramework>net10.0</TargetFramework><RuntimeFrameworkVersion>10.0.10</RuntimeFrameworkVersion><LangVersion>14.0</LangVersion>
<AssemblyName>{escape(module['assembly'])}</AssemblyName><EnableDefaultCompileItems>false</EnableDefaultCompileItems><Nullable>enable</Nullable><ImplicitUsings>enable</ImplicitUsings>
<AllowUnsafeBlocks>{str(module.get('unsafe', False)).lower()}</AllowUnsafeBlocks><DefineConstants>{escape(defines)}</DefineConstants><Deterministic>true</Deterministic>
</PropertyGroup><ItemGroup>{includes}{references}<ProjectReference Include="{escape(engine / 'Engine/Managed/HEngine.ScriptCore/HEngine.ScriptCore.csproj')}" /></ItemGroup></Project>''', encoding="utf-8")
        command = [dotnet, "build", str(csproj), "-c", configuration, "-o", str(directory / "output"), "--artifacts-path", str(directory / "artifacts"),
            "--disable-build-servers", "-p:UseSharedCompilation=false", "-nodeReuse:false", f"-p:GeneratedApiDir={directory / 'api'}"]
        report_status(module, "compiling")
        with (directory / "compiler.log").open("w", encoding="utf-8") as log:
            completed = subprocess.run(command, cwd=engine / "Engine/Managed", stdout=log, stderr=subprocess.STDOUT)
        report_diagnostics(module, directory / "compiler.log")
        if completed.returncode:
            report_status(module, "failed")
            raise ValueError(f"Module {module['id']} compilation failed: {directory / 'compiler.log'}")
        if inputs(project, module) != item["inputs"]:
            raise ValueError(f"Module inputs changed during compilation: {module['id']}")
        dll = directory / "output" / (module["assembly"] + ".dll")
        receipt = {"schema": 1, "id": module["id"], "assembly": module["assembly"], "identity": key, "sha256": digest(dll.read_bytes()), "inputs": item["inputs"], "references": module.get("references", []), "category": module["category"]}
        receipt_path.write_text(json.dumps(receipt, indent=2), encoding="utf-8")
        outputs[module["id"]] = dll
        results.append({**receipt, "cached": False, "dll": str(dll)})
        report_status(module, "built")
    return {"schema": 1, "platform": platform, "configuration": configuration, "sdk": sdk, "api": api, "modules": results}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", type=Path, required=True)
    parser.add_argument("--engine", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--platform", choices=["Windows", "Web", "WeChat", "macOS"], default="Windows")
    parser.add_argument("--configuration", default="Release")
    parser.add_argument("--category", action="append", choices=["Runtime", "Editor", "Tests"])
    parser.add_argument("--dotnet", default="dotnet")
    args = parser.parse_args()
    args.work = args.work.resolve()
    try:
        result = build(args.project.resolve(), args.engine.resolve(), args.work, args.platform, args.configuration, set(args.category or ["Runtime"]), args.dotnet)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print("HE_SCRIPT_MODULE_DIAGNOSTIC=" + json.dumps({"severity": "error", "code": "HEMOD001", "module": "module graph/build",
            "file": "", "line": 0, "column": 0, "message": str(error)}), flush=True)
        raise SystemExit(1)
    args.work.mkdir(parents=True, exist_ok=True)
    (args.work / "modules.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps({"built": sum(not m["cached"] for m in result["modules"]), "cached": sum(m["cached"] for m in result["modules"]), "manifest": str(args.work / 'modules.json')}))
