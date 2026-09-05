# WebGPU Backend

The WebGPU backend is experimental. It uses Dawn as the native WebGPU
implementation and WGSL as the shader source format.

## Requirements

- CMake 3.15 or newer
- A C++17-capable toolchain
- `BUILD_WEBGPU=ON`
- Dawn, prepared with `scripts/fetch_dawn.py` or supplied as an existing checkout
- `BUILD_TOOL=ON` when rebuilding WGSL shader assets with `ShaderTranspiler`

On Windows, Dawn normally uses D3D12. Vulkan or other Dawn adapters may be
available depending on the local Dawn build and runtime environment.

## CMake Options

| Option | Default | Description |
| --- | --- | --- |
| `BUILD_WEBGPU` | `OFF` | Enable the WebGPU backend |
| `WEBGPU_DAWN_SOURCE_DIR` | empty | Use an existing Dawn checkout |
| `WEBGPU_DAWN_BUILD_SAMPLES` | `OFF` | Build Dawn sample executables |
| `WEBGPU_DAWN_FORCE_SYSTEM_COMPONENT_LOAD` | `ON` | Let Dawn load Windows system components such as `d3dcompiler_47.dll` from System32 |

## Getting Dawn

### Recommended Flow

Dawn should be fetched before configuring LLGI. The helper script clones Dawn
into `thirdparty/dawn` and runs Dawn's dependency fetch script:

```bash
python scripts/fetch_dawn.py
```

By default, the helper passes `--no-shallow` to Dawn's dependency fetch script.
This is slower, but avoids partially-created dependency checkouts on Git
installations that do not support Dawn's shallow fetch flow well. Use shallow
dependency fetches only when the local Git/depot_tools setup is known to handle
them:

```bash
python scripts/fetch_dawn.py --shallow-dependencies
```

Pin a Dawn revision for reproducible builds:

```bash
python scripts/fetch_dawn.py --revision <branch-tag-or-commit>
```

Then configure LLGI:

```bash
cmake -S . -B build-webgpu \
  -DBUILD_WEBGPU=ON \
  -DBUILD_TEST=ON \
  -DBUILD_TOOL=ON
cmake --build build-webgpu --config Release
```

When `BUILD_WEBGPU=ON`, LLGI enables Dawn's CMake install/export metadata so
parent projects can generate their own install exports without disabling install
rules.

### Existing Dawn Checkout

You can use a separate Dawn checkout if its dependencies have already been
fetched:

```bash
cmake -S . -B build-webgpu \
  -DBUILD_WEBGPU=ON \
  -DBUILD_TEST=ON \
  -DBUILD_TOOL=ON \
  -DWEBGPU_DAWN_SOURCE_DIR=/path/to/dawn
cmake --build build-webgpu --config Release
```

### `thirdparty/dawn`

The repository checks `thirdparty/dawn` automatically. This is useful when you
want Dawn to live inside the LLGI working tree. Prefer the helper script above,
or run Dawn's official setup manually:

```bash
cd thirdparty
git clone https://dawn.googlesource.com/dawn dawn
cd dawn
cp scripts/standalone.gclient .gclient
gclient sync
```

Then configure LLGI with `-DBUILD_WEBGPU=ON`.

## Running Tests

Build `LLGI_Test` with `BUILD_TEST=ON`, then pass `--webgpu`:

```bash
# Windows
build-webgpu\src_test\Release\LLGI_Test.exe --webgpu
build-webgpu\src_test\Release\LLGI_Test.exe --webgpu --filter=SimpleRender.*

# Linux / macOS
./build-webgpu/src_test/LLGI_Test --webgpu
./build-webgpu/src_test/LLGI_Test --webgpu --filter=SimpleRender.*
```

Some environments need a visible GPU session for Dawn to create a WebGPU device.
Headless CI may need Dawn-specific setup.

## Browser Smoke Test

The native test path above uses Dawn directly. To compile the WebGPU backend to
WebAssembly and run it in a real browser WebGPU implementation, use the browser
test flow in `docs/WebGPU_Browser_Test.md`.

## Shader Generation

`ShaderTranspiler` can emit WGSL and compiled WGSL blobs for WebGPU tests.

```bash
cmake -S . -B build-webgpu \
  -DBUILD_TOOL=ON \
  -DBUILD_WEBGPU=ON
cmake --build build-webgpu --config Release --target ShaderTranspiler

python scripts/transpile.py src_test/Shaders/
```

Generated WebGPU shaders are stored under:

- `src_test/Shaders/WebGPU/`
- `src_test/Shaders/WebGPU_Compiled/`

The compiled files use an LLGI header followed by WGSL text. Runtime WebGPU
shader creation expects WGSL or this compiled WGSL format; legacy runtime WGSL
rewrites are not applied.

## Current Limitations

- The backend is experimental and is not enabled by default.
- Dawn API and Tint WGSL output can change. Prefer pinning
  `scripts/fetch_dawn.py --revision` for stable builds.
- The WebGPU backend currently depends on Dawn CMake targets
  `dawn::webgpu_dawn` or `webgpu_dawn`.
- WebGPU shader assets should be regenerated when the shader transpiler or Tint
  revision changes.
- `LLGI::CreateCompiler(DeviceType::WebGPU)` is not a runtime HLSL compiler; use
  `ShaderTranspiler` to generate WGSL assets.
- External-device construction APIs can run without an owned `wgpu::Instance`.
  In that mode, some wait processing is limited to the supplied Dawn objects.

## Troubleshooting

- If configure fails because Dawn is missing, run `python scripts/fetch_dawn.py`,
  add `thirdparty/dawn`, or set `WEBGPU_DAWN_SOURCE_DIR`.
- If Dawn dependency sync fails, rerun `python scripts/fetch_dawn.py` so the
  helper uses Dawn's `--no-shallow` dependency mode. Install `depot_tools` and
  ensure `gclient` is available on `PATH` if you use Dawn's official `gclient`
  flow instead.
- If WebGPU device creation fails on Windows with `d3dcompiler_47.dll`, rebuild
  with `WEBGPU_DAWN_FORCE_SYSTEM_COMPONENT_LOAD=ON`.
- If shader creation fails, regenerate WGSL with the current `ShaderTranspiler`
  and check that the generated shaders are under `src_test/Shaders/WebGPU/` and
  `src_test/Shaders/WebGPU_Compiled/`.
