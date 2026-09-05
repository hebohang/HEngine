# LLGI

LLGI is a cross-platform graphics abstraction library that provides a common
API for DirectX 12, Metal, and Vulkan.

This repository currently contains:

- `LLGI`: the core static library
- `LLGI_Test`: rendering and shader tests (`BUILD_TEST=ON`)
- `example_glfw`, `example_imgui`, `example_GPUParticle`: sample applications
  (`BUILD_EXAMPLE=ON`)
- `ShaderTranspiler`: shader conversion tool (`BUILD_TOOL=ON`)

## Backend and platform support

| Platform | Default backend | Optional backends | Notes |
| --- | --- | --- | --- |
| Windows | DirectX 12 | Vulkan | CI builds both x86 and x64 configurations. |
| macOS | Metal | - | CI builds with `CMAKE_OSX_DEPLOYMENT_TARGET=10.15`. |
| iOS | Metal | - | CI includes an iOS build configuration. |
| Linux | Vulkan | - | `BUILD_VULKAN` is forced to `ON` on Linux. |

## Repository layout

| Path | Description |
| --- | --- |
| `src/` | LLGI library sources and public headers |
| `src_test/` | Test executable and shader test assets |
| `examples/` | GLFW, Dear ImGui, and GPU particle samples |
| `tools/` | Shader transpiler sources |
| `scripts/transpile.py` | Helper script to batch-convert shader assets |
| `thirdparty/` | Bundled third-party dependencies used by Vulkan compiler/tool builds |

## Getting the source

```bash
git clone https://github.com/effekseer/LLGI.git
cd LLGI
git submodule update --init --recursive
```

## Build requirements

- CMake 3.15 or newer
- A C++ toolchain for your platform (`Visual Studio`, `Xcode`, `clang`, or `gcc`)
- On Linux, Vulkan and X11 development packages
- When `BUILD_VULKAN_COMPILER` or `BUILD_TOOL` is enabled, the bundled
  `glslang` and `SPIRV-Cross` submodules are used by default

Linux packages used in CI:

```bash
sudo apt-get update
sudo apt-get install -y \
  libx11-dev \
  libxrandr-dev \
  libxi-dev \
  libxinerama-dev \
  libxcursor-dev \
  libudev-dev \
  libx11-xcb-dev \
  libglu1-mesa-dev \
  mesa-common-dev \
  libvulkan-dev
```

## Common CMake options

| Option | Default | Description |
| --- | --- | --- |
| `BUILD_TEST` | `OFF` | Build `LLGI_Test` |
| `BUILD_EXAMPLE` | `OFF` | Build sample applications |
| `BUILD_TOOL` | `OFF` | Build `ShaderTranspiler` |
| `BUILD_WEBGPU` | `OFF` | Enable the experimental WebGPU backend. See [docs/WebGPU.md](docs/WebGPU.md). |
| `BUILD_VULKAN` | `OFF` (`ON` on Linux) | Enable the Vulkan backend |
| `BUILD_VULKAN_COMPILER` | `OFF` | Enable Vulkan shader compilation support in `LLGI::CreateCompiler` |
| `USE_CREATE_COMPILER_FUNCTION` | `ON` | Keep `LLGI::CreateCompiler` enabled |
| `USE_MSVC_RUNTIME_LIBRARY_DLL` | `ON` | Use the DLL MSVC runtime (`/MD`) |

## Build

### Windows (DirectX 12)

```bash
cmake -S . -B build -DBUILD_TEST=ON -DBUILD_EXAMPLE=ON
cmake --build build --config Release
```

For a 32-bit build:

```bash
cmake -S . -B build -A Win32 -DBUILD_TEST=ON
cmake --build build --config Release
```

### Windows (DirectX 12 + Vulkan tools)

```bash
cmake -S . -B build \
  -DBUILD_TEST=ON \
  -DBUILD_EXAMPLE=ON \
  -DBUILD_TOOL=ON \
  -DBUILD_VULKAN=ON \
  -DBUILD_VULKAN_COMPILER=ON
cmake --build build --config Release
```

### macOS

```bash
cmake -S . -B build -G Xcode \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=10.15 \
  -DBUILD_TEST=ON \
  -DBUILD_EXAMPLE=ON
cmake --build build --config Release
```

### iOS

```bash
cmake -S . -B build-ios -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 \
  -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
cmake --build build-ios --config Release
```

### Linux (Vulkan)

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TEST=ON \
  -DBUILD_EXAMPLE=ON \
  -DBUILD_TOOL=ON \
  -DBUILD_VULKAN_COMPILER=ON
cmake --build build
```

On Linux, `BUILD_VULKAN` is enabled automatically by the top-level
`CMakeLists.txt`.

### WebGPU (experimental)

The WebGPU backend uses Dawn and is still experimental. Build instructions,
Dawn setup, test commands, shader notes, and current limitations are documented
in [docs/WebGPU.md](docs/WebGPU.md).

## Install

```bash
cmake --install build --prefix <install-prefix>
```

For multi-config generators such as Visual Studio or Xcode, add
`--config Release`.

The install step exports the `LLGI` static library, public headers, and CMake
package files under `lib/cmake`.

## Running tests

`LLGI_Test` is available when `BUILD_TEST=ON`.

Default device selection:

- Windows: DirectX 12
- macOS/iOS: Metal
- Linux: Vulkan

Examples:

```bash
# Linux / macOS
./build/src_test/LLGI_Test
./build/src_test/LLGI_Test --filter=SimpleRender.*

# Windows
build\src_test\Release\LLGI_Test.exe
build\src_test\Release\LLGI_Test.exe --filter=Compile.*
build\src_test\Release\LLGI_Test.exe --vulkan
build\src_test\Release\LLGI_Test.exe --webgpu
```

If you want Vulkan shader compilation through
`LLGI::CreateCompiler(DeviceType::Vulkan)` or the `Compile.*` tests on Vulkan,
configure with `-DBUILD_VULKAN_COMPILER=ON`.

## Examples

When `BUILD_EXAMPLE=ON`, the following targets are built:

- `example_glfw`: minimal clear/present flow using GLFW
- `example_imgui`: Dear ImGui integration on top of LLGI and GLFW
- `example_GPUParticle`: GPU particle sample

The smallest end-to-end sample is in `examples/glfw/main.cpp`.

## Shader tools

When `BUILD_TOOL=ON`, the repository builds `ShaderTranspiler`.

The helper script below uses the built tool to batch-convert shader assets:

```bash
python scripts/transpile.py src_test/Shaders/
python scripts/transpile.py examples/GPUParticle/Shaders/
```

Additional notes are available in [tools/README.md](tools/README.md).

## Minimal usage

```cpp
auto window = std::unique_ptr<LLGI::Window>(LLGI::CreateWindow("LLGI", {1280, 720}));

LLGI::PlatformParameter parameter;
parameter.Device = LLGI::DeviceType::Default;

auto platform = LLGI::CreateSharedPtr(LLGI::CreatePlatform(parameter, window.get()));
auto graphics = LLGI::CreateSharedPtr(platform->CreateGraphics());
auto memoryPool = LLGI::CreateSharedPtr(graphics->CreateSingleFrameMemoryPool(1024 * 1024, 128));
auto commandList = LLGI::CreateSharedPtr(graphics->CreateCommandList(memoryPool.get()));
```

For a complete runnable example, see `examples/glfw/main.cpp`.

## License

LLGI is distributed under the zlib license. See [LICENSE](LICENSE).
