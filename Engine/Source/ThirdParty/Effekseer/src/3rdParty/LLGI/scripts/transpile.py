# Usage: python scripts/transpile.py {target}
# e.g. python scripts/transpile.py src_test/Shaders/
#      python scripts/transpile.py examples/GPUParticle/Shaders/

import argparse
import glob
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
LLGI_ROOT = SCRIPT_DIR.parent

from shader_pipeline import find_shader_transpiler, run_shader_transpiler


def resolve_target_directory(target):
    target_path = Path(target)
    if not target_path.is_absolute():
        target_path = Path.cwd() / target_path
    return target_path.resolve()


def collect_sources(target_directory):
    source_dir = target_directory / "HLSL_DX12"
    return {
        "--vert": sorted(source_dir.glob("*.vert")),
        "--frag": sorted(source_dir.glob("*.frag")),
        "--comp": sorted(source_dir.glob("*.comp")),
    }


def transpile_matrix(shader_transpiler, target_directory, sources):
    for target, directory, shader_model in [
        ("metal", "Metal", None),
        ("vulkan-glsl", "GLSL_VULKAN", None),
        ("glsl", "GLSL_GL", None),
        ("glsl", "GLSL_GL_450", 450),
    ]:
        for stage, paths in sources.items():
            for source in paths:
                run_shader_transpiler(
                    shader_transpiler,
                    source,
                    target_directory / directory / source.name,
                    stage,
                    target,
                    shader_model=shader_model,
                )


def transpile_webgpu(shader_transpiler, target_directory, sources):
    for stage, paths in sources.items():
        for source in paths:
            compiled_output = target_directory / "WebGPU_Compiled" / source.name
            compiled_output.parent.mkdir(parents=True, exist_ok=True)
            run_shader_transpiler(
                shader_transpiler,
                source,
                target_directory / "WebGPU" / source.name,
                stage,
                "wgsl",
                extra_args=["--compiled-output", compiled_output],
            )


def find_glslang_validator():
    glslang_validator = shutil.which("glslangValidator")
    if glslang_validator is not None:
        return glslang_validator

    candidates = []
    vulkan_sdk = os.environ.get("VULKAN_SDK")
    if vulkan_sdk is not None:
        candidates.append(Path(vulkan_sdk) / "Bin" / "glslangValidator.exe")

    if platform.system() == "Windows":
        vulkan_sdk_root = Path("C:/VulkanSDK")
        if vulkan_sdk_root.exists():
            candidates.extend(sorted(vulkan_sdk_root.glob("*/Bin/glslangValidator.exe"), reverse=True))

    for candidate in candidates:
        if candidate.exists():
            return str(candidate)

    return None


def generate_spirv(target_directory):
    if platform.system() == "Linux":
        return

    glslang_validator = find_glslang_validator()
    if glslang_validator is None:
        raise FileNotFoundError("glslangValidator was not found in PATH.")

    spirv_dir = target_directory / "SPIRV"
    spirv_dir.mkdir(parents=True, exist_ok=True)

    for source in sorted(glob.glob(str(target_directory / "GLSL_VULKAN" / "*.*"))):
        source_path = Path(source)
        if source_path.suffix not in [".vert", ".frag", ".comp"]:
            continue
        subprocess.run(
            [
                glslang_validator,
                str(source_path),
                "-e",
                "main",
                "-V",
                "-o",
                str(spirv_dir / (source_path.name + ".spv")),
            ],
            check=True,
        )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("target", help="target directory")
    parser.add_argument("--skip-webgpu", action="store_true", help="skip WebGPU shader generation")
    parser.add_argument("--require-webgpu", action="store_true", help="fail if WebGPU shader generation fails")
    args = parser.parse_args()

    target_directory = resolve_target_directory(args.target)
    shader_transpiler = find_shader_transpiler(LLGI_ROOT)
    sources = collect_sources(target_directory)

    transpile_matrix(shader_transpiler, target_directory, sources)
    generate_spirv(target_directory)

    if not args.skip_webgpu:
        try:
            transpile_webgpu(shader_transpiler, target_directory, sources)
        except subprocess.CalledProcessError:
            if args.require_webgpu:
                raise
            print(
                "WebGPU shader generation failed. "
                "Rebuild ShaderTranspiler with BUILD_WEBGPU=ON to generate WebGPU shaders.",
                file=sys.stderr,
            )


if __name__ == "__main__":
    main()
