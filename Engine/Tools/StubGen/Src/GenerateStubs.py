"""Generate from the loaded HEngine module; validate before publishing files."""
import ast
import hashlib
import inspect
import json
from pathlib import Path
import sys

from StubGen.Src import Pybind11Stubgen as stubgen


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_hashes(repository):
    files = list((repository / "Engine/Source/Plugins/PythonScript/Binding").glob("*"))
    files += list((repository / "Engine/Tools/StubGen/Src").rglob("*.py"))
    files += list((repository / "Engine/Source/Runtime/EcsFramework/Component").rglob("*.h"))
    return {p.relative_to(repository).as_posix(): digest(p)
            for p in sorted(files) if p.is_file()}


def declared_symbols(text):
    result = set()
    def visit(nodes, prefix=""):
        for node in nodes:
            if isinstance(node, (ast.FunctionDef, ast.ClassDef)):
                result.add(prefix + node.name)
                if isinstance(node, ast.ClassDef):
                    visit(node.body, prefix + node.name + ".")
            elif isinstance(node, ast.AnnAssign) and isinstance(node.target, ast.Name):
                result.add(prefix + node.target.id)
            elif isinstance(node, ast.Assign):
                result.update(prefix + target.id for target in node.targets if isinstance(target, ast.Name))
            elif isinstance(node, ast.ImportFrom):
                result.update(prefix + (n.asname or n.name) for n in node.names)
    visit(ast.parse(text).body)
    return result


def public_symbols(module):
    result = set()
    for name, value in vars(module).items():
        if name.startswith("_"):
            continue
        result.add(name)
        if inspect.isclass(value) and value.__module__ == module.__name__:
            result.update(name + "." + member for member in vars(value)
                          if not member.startswith("_"))
    return result


def write_if_changed(path, content):
    encoded = content.encode("utf-8")
    if path.exists() and path.read_bytes() == encoded:
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_bytes(encoded)
    temporary.replace(path)
    return True


def generate(output=None, repository=None, module=None, build_identity=None):
    repository = Path(repository or Path(__file__).resolve().parents[4])
    output = Path(output or repository / "Engine/Tools/HEngineStub")
    if module is None:
        import HEngine
        module = HEngine
    if build_identity is None:
        executable = Path(sys.executable) if sys.executable else None
        build_identity = {"configuration": "unknown", "backend": "unknown",
                          "executable": str(executable) if executable else "unknown",
                          "sha256": digest(executable) if executable and executable.is_file() else "unknown"}
    stubgen._visited_objects.clear()
    stubgen.unresolved_cpp_types.clear()
    stubgen.FunctionSignature.n_invalid_signatures = 0
    stubgen.FunctionSignature.n_invalid_default_values = 0
    generator = stubgen.ModuleStubsGenerator(module)
    generator.parse()
    files, exports = {}, {}
    def collect(current):
        name = current.module.__name__
        content = "\n".join(current.to_lines())
        missing = public_symbols(current.module) - declared_symbols(content)
        if missing:
            raise ValueError("Missing public symbols in {}: {}".format(name, sorted(missing)))
        files[name.replace(".", "/") + "/__init__.pyi"] = content
        exports[name] = sorted(public_symbols(current.module))
        for child in current.submodules:
            collect(child)
    collect(generator)
    if stubgen.FunctionSignature.n_invalid_signatures:
        raise ValueError("Invalid binding signatures: {}".format(
            stubgen.FunctionSignature.n_invalid_signatures))
    manifest = {
        "schemaVersion": 1,
        "module": module.__name__,
        "pythonVersion": sys.version.split()[0],
        "buildIdentity": build_identity,
        "availability": {"GM": hasattr(module, "GM"),
                         "note": "Only symbols present in this loaded build are exported."},
        "sources": source_hashes(repository),
        "exports": exports,
        "unregisteredCppTypes": sorted(stubgen.unresolved_cpp_types),
        "unregisteredTypePolicy": "typing.Any; no runtime support is implied",
        "files": {name: hashlib.sha256(value.encode("utf-8")).hexdigest()
                  for name, value in files.items()},
    }
    changed = sum(write_if_changed(output / name, value) for name, value in files.items())
    manifest_path = output / "stub-manifest.json"
    deleted = 0
    if manifest_path.exists():
        previous = json.loads(manifest_path.read_text(encoding="utf-8"))
        for name in previous.get("files", {}):
            target = (output / name).resolve()
            if name not in files and output.resolve() in target.parents and target.is_file():
                target.unlink()
                deleted += 1
    write_if_changed(manifest_path, json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    return {"changed": changed, "deleted": deleted, "modules": len(exports),
            "unregisteredCppTypes": manifest["unregisteredCppTypes"]}


class GenerateStubs:
    def Start(self, gameObject):
        print(json.dumps(generate(), sort_keys=True))
