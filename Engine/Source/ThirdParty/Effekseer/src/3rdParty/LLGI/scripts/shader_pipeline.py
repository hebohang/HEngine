import subprocess
import sys
from pathlib import Path


STAGE_ARGS = {
    "vert": "--vert",
    "vertex": "--vert",
    "--vert": "--vert",
    "frag": "--frag",
    "pixel": "--frag",
    "--frag": "--frag",
    "comp": "--comp",
    "compute": "--comp",
    "--comp": "--comp",
}

TARGET_ARGS = {
    "glsl": ["-G"],
    "vulkan-glsl": ["-V"],
    "metal": ["-M"],
    "hlsl": ["-H"],
    "wgsl": ["-W"],
    "spv": ["-S"],
    "vulkan-spv": ["-S", "--vulkan-spv"],
    "-G": ["-G"],
    "-V": ["-V"],
    "-M": ["-M"],
    "-H": ["-H"],
    "-W": ["-W"],
    "-S": ["-S"],
}

def fail_if_missing(path, label):
    path = Path(path)
    if not path.exists():
        print(f"{label} was not found: {path}", file=sys.stderr)
        sys.exit(1)


def find_shader_transpiler(llgi_root, extra_candidates=None):
    llgi_root = Path(llgi_root)
    candidates = []

    def add_executable_candidates(directory):
        directory = Path(directory)
        candidates.append(directory / "ShaderTranspiler.exe")
        candidates.append(directory / "ShaderTranspiler")

    if extra_candidates is not None:
        candidates.extend(Path(p) for p in extra_candidates)

    if len(llgi_root.parents) >= 5:
        upstream_root = llgi_root.parents[4] / "Effekseer"
        upstream_llgi_root = upstream_root / "Dev" / "Cpp" / "3rdParty" / "LLGI"
        add_executable_candidates(upstream_llgi_root / "build-tool" / "tools" / "ShaderTranspiler" / "Release")
        add_executable_candidates(upstream_llgi_root / "build-tool" / "tools" / "ShaderTranspiler" / "Debug")
        add_executable_candidates(upstream_llgi_root / "build" / "tools" / "ShaderTranspiler" / "Debug")
        add_executable_candidates(
            upstream_root / "build" / "Dev" / "Cpp" / "3rdParty" / "LLGI" / "tools" / "ShaderTranspiler" / "Debug"
        )

    add_executable_candidates(llgi_root / "build_shadertranspiler_upstream" / "tools" / "ShaderTranspiler" / "Release")
    add_executable_candidates(llgi_root / "build" / "tools" / "ShaderTranspiler" / "Release")
    add_executable_candidates(llgi_root / "build" / "tools" / "ShaderTranspiler" / "Debug")
    add_executable_candidates(llgi_root / "src_test" / "Shaders")
    add_executable_candidates(llgi_root / "scripts")

    for candidate in candidates:
        if candidate.exists():
            return candidate

    print("ShaderTranspiler was not found.", file=sys.stderr)
    for candidate in candidates:
        print(f"  {candidate}", file=sys.stderr)
    sys.exit(1)


def stage_from_path(path):
    name = Path(path).name.lower()
    if name.endswith(".vert") or name.endswith("_vs.fx"):
        return "--vert"
    if name.endswith(".frag") or name.endswith("_ps.fx"):
        return "--frag"
    if name.endswith(".comp") or name.endswith("_cs.fx"):
        return "--comp"
    raise ValueError(f"Cannot infer shader stage from {path}")


def run_shader_transpiler(
    shader_transpiler,
    input_path,
    output_path,
    stage,
    target,
    include_dirs=None,
    macros=None,
    shader_model=None,
    extra_args=None,
):
    output_path = Path(output_path)
    line_ending = "\n"
    trailing_newline_count = 1
    if output_path.suffix.lower() != ".spv" and output_path.exists():
        with output_path.open("r", encoding="utf-8", newline="") as previous_output:
            previous_source = previous_output.read()
        if "\r\n" in previous_source:
            line_ending = "\r\n"
        elif "\r" in previous_source:
            line_ending = "\r"
        previous_suffix = previous_source[len(previous_source.rstrip("\r\n")) :]
        previous_suffix = previous_suffix.replace("\r\n", "\n").replace("\r", "\n")
        trailing_newline_count = len(previous_suffix)

    command = [
        str(shader_transpiler),
        STAGE_ARGS[stage],
    ]
    command += TARGET_ARGS[target]
    command += [
        "--input",
        str(input_path),
        "--output",
        str(output_path),
    ]

    for include_dir in include_dirs or []:
        command += ["-I", str(include_dir)]

    for name, value in macros or []:
        command += ["-D", str(name), str(value)]

    if shader_model is not None:
        command += ["--sm", str(shader_model)]

    if extra_args is not None:
        command += [str(arg) for arg in extra_args]

    output_path.parent.mkdir(parents=True, exist_ok=True)
    print("Transpile " + str(input_path), flush=True)
    subprocess.run(command, check=True)

    if output_path.suffix.lower() != ".spv":
        with output_path.open("r", encoding="utf-8", newline="") as generated_output:
            source = generated_output.read()
        source = source.replace("\r\n", "\n").replace("\r", "\n").rstrip("\n")
        source = source.replace("\n", line_ending)
        with output_path.open("w", encoding="utf-8", newline="") as output:
            output.write(source + line_ending * trailing_newline_count)
