# WebGPU Browser Test

This document explains how to build and run LLGI's browser-only WebGPU tests.
The browser test target compiles LLGI to WebAssembly with Emscripten, runs it in
an installed Chrome or Edge browser, and accesses the browser WebGPU API through
Emdawnwebgpu.

The native WebGPU path in `docs/WebGPU.md` uses Dawn directly. This document is
for the browser runtime, which verifies:

- Emscripten compilation and linking
- Emdawnwebgpu C/C++ bindings
- browser `navigator.gpu` adapter and device creation
- WGSL shader module creation
- render and compute pipeline creation
- offscreen render target readback
- storage buffer compute readback
- browser canvas surface presentation

## Requirements

- CMake 3.15 or newer
- Git
- Node.js 22 or newer
- Emscripten 4.x or newer with `--use-port=emdawnwebgpu`
- An installed WebGPU-capable Chrome or Edge browser

The automated runner does not use Playwright. It starts an installed browser
directly and controls it through the Chrome DevTools Protocol.

WebGPU also requires a secure context. The runner serves the generated files
from `127.0.0.1`; do not open `LLGI_Test.html` directly with `file://`.

## Quick Start

From the repository root, after activating emsdk:

```bash
emcmake cmake -S . -B build-webgpu-browser \
  -DBUILD_WEBGPU=ON \
  -DBUILD_WEBGPU_BROWSER_TEST=ON \
  -DBUILD_TEST=ON
cmake --build build-webgpu-browser --target LLGI_Test
node src_test/browser/run_webgpu_browser_test.mjs \
  build-webgpu-browser/src_test/LLGI_Test.html
```

PowerShell:

```powershell
emcmake cmake -S . -B build-webgpu-browser `
  -DBUILD_WEBGPU=ON `
  -DBUILD_WEBGPU_BROWSER_TEST=ON `
  -DBUILD_TEST=ON
cmake --build build-webgpu-browser --target LLGI_Test
node src_test/browser/run_webgpu_browser_test.mjs `
  build-webgpu-browser/src_test/LLGI_Test.html
```

If Chrome or Edge is installed in a non-standard location, set `CHROME_PATH`
before running the Node.js runner:

```powershell
$env:CHROME_PATH = "C:\Program Files (x86)\Google\Chrome\Application\chrome.exe"
```

On macOS, the runner auto-detects the standard Chrome and Edge application
paths. If needed, set the browser path explicitly:

```bash
export CHROME_PATH="/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
```

A successful run ends with:

```text
LLGI_TEST_PASS completed
```

## Install Emscripten

Install emsdk from the official repository:

```bash
git clone https://github.com/emscripten-core/emsdk.git
cd emsdk
./emsdk install latest
./emsdk activate latest
source ./emsdk_env.sh
```

On Windows PowerShell:

```powershell
git clone https://github.com/emscripten-core/emsdk.git
cd emsdk
.\emsdk install latest
.\emsdk activate latest
.\emsdk_env.ps1
```

Check the installation:

```bash
emcc --version
emcmake --version
```

Run the emsdk environment script again whenever you open a new shell.

## Select a Browser

The runner auto-detects common Chrome and Edge installation paths. You can
override the browser path with `CHROME_PATH`:

```powershell
$env:CHROME_PATH = "C:\Program Files (x86)\Google\Chrome\Application\chrome.exe"
```

Common Windows paths are:

```powershell
$env:CHROME_PATH = "C:\Program Files\Google\Chrome\Application\chrome.exe"
$env:CHROME_PATH = "C:\Program Files (x86)\Google\Chrome\Application\chrome.exe"
$env:CHROME_PATH = "C:\Program Files\Microsoft\Edge\Application\msedge.exe"
$env:CHROME_PATH = "C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"
```

On some environments, Playwright's downloaded Chromium/headless shell may expose
`navigator.gpu` but still return no adapter from `navigator.gpu.requestAdapter()`.
Use installed Chrome or Edge when running these tests.

## Configure

Use Emscripten's CMake wrapper:

```bash
emcmake cmake -S . -B build-webgpu-browser \
  -DBUILD_WEBGPU=ON \
  -DBUILD_WEBGPU_BROWSER_TEST=ON \
  -DBUILD_TEST=ON
```

PowerShell:

```powershell
emcmake cmake -S . -B build-webgpu-browser `
  -DBUILD_WEBGPU=ON `
  -DBUILD_WEBGPU_BROWSER_TEST=ON `
  -DBUILD_TEST=ON
```

`BUILD_WEBGPU_BROWSER_TEST=ON` requires the Emscripten toolchain and builds a
browser-focused `LLGI_Test.html`. It does not require a native Dawn checkout.

## Build

```bash
cmake --build build-webgpu-browser --target LLGI_Test
```

The generated files are:

```text
build-webgpu-browser/src_test/LLGI_Test.html
build-webgpu-browser/src_test/LLGI_Test.js
build-webgpu-browser/src_test/LLGI_Test.wasm
build-webgpu-browser/src_test/LLGI_Test.data
```

On Windows, if emsdk is installed on a non-`C:` drive, keep `EM_CACHE` on the
same drive as emsdk. This avoids Emscripten port-build failures caused by
cross-drive relative paths:

```powershell
$env:EM_CACHE = "D:\emscripten-cache-llgi"
cmake --build build-webgpu-browser --target LLGI_Test
```

For CI, cache `EM_CACHE` between runs to avoid rebuilding Emscripten system
libraries and the Emdawnwebgpu port every time.

## Run Tests

Run the browser harness from the repository root:

```bash
node src_test/browser/run_webgpu_browser_test.mjs \
  build-webgpu-browser/src_test/LLGI_Test.html
```

PowerShell:

```powershell
node src_test/browser/run_webgpu_browser_test.mjs `
  build-webgpu-browser/src_test/LLGI_Test.html
```

The runner:

- starts a temporary `127.0.0.1` HTTP server
- disables caching for generated `.html`, `.js`, `.wasm`, and `.data` files
- launches installed Chrome or Edge in headless mode
- passes WebGPU-friendly browser flags
- connects to the browser through the Chrome DevTools Protocol
- forwards browser console and page errors
- waits for the Emscripten module to report completion
- exits non-zero on failure

The default filter is:

```text
WebGPUBrowser.*
```

Run one test with `--filter`:

```bash
node src_test/browser/run_webgpu_browser_test.mjs \
  build-webgpu-browser/src_test/LLGI_Test.html \
  --filter=WebGPUBrowser.ScreenPresentation
```

## View a Canvas Test

Most tests render offscreen and validate the result through readback. To see the
visible canvas presentation test, serve the generated files:

```bash
cd build-webgpu-browser/src_test
python -m http.server 8000
```

Open:

```text
http://localhost:8000/LLGI_Test.html?filter=WebGPUBrowser.ScreenPresentation
```

You should see a blue canvas with a colored polygon. Open browser DevTools and
check the Console for:

```text
Start : WebGPUBrowser.ScreenPresentation
LLGI_TEST_PASS completed
```

## CTest

When Node.js is found during CMake configure, CMake registers:

```bash
ctest --test-dir build-webgpu-browser -R LLGI_WebGPU_Browser --output-on-failure
```

The CTest entry uses the same Node.js runner. It expects a WebGPU-capable Chrome
or Edge executable. Set `CHROME_PATH` in the test environment when
auto-detection is not enough.

## Test Cases

- `WebGPUBrowser.ComputeCompile`: loads a WGSL compute shader and compiles a
  compute pipeline
- `WebGPUBrowser.ComputeDispatch`: dispatches a storage-buffer compute shader
  and verifies mapped readback data
- `WebGPUBrowser.OffscreenRender`: renders to an offscreen texture
- `WebGPUBrowser.RenderReadback`: clears an offscreen render target and verifies
  copied pixel data
- `WebGPUBrowser.TextureAndConstantRender`: uploads texture and uniform data,
  renders, and verifies readback pixels
- `WebGPUBrowser.ScreenPresentation`: renders through
  `PlatformWebGPU::GetCurrentScreen` to the browser canvas

Browser readback uses Asyncify so C++ test code can wait for `MapAsync` and
`Queue::OnSubmittedWorkDone` callbacks while the browser event loop continues to
run. The browser WebGPU instance enables `TimedWaitAny` because the readback
paths use timeout-based `Instance::WaitAny` calls.

## CI Notes

A typical CI flow is:

```bash
emcmake cmake -S . -B build-webgpu-browser \
  -DBUILD_WEBGPU=ON \
  -DBUILD_WEBGPU_BROWSER_TEST=ON \
  -DBUILD_TEST=ON
cmake --build build-webgpu-browser --target LLGI_Test
node src_test/browser/run_webgpu_browser_test.mjs \
  build-webgpu-browser/src_test/LLGI_Test.html
```

The runner passes these browser flags:

- `--headless=new`
- `--enable-unsafe-webgpu`
- `--ignore-gpu-blocklist`
- `--use-angle=d3d11`

`--use-angle=d3d11` is important on the tested Windows environment. Vulkan or
SwiftShader flags can leave `navigator.gpu.requestAdapter()` returning `null`.

## Troubleshooting

### `navigator.gpu is not available`

Use `http://localhost`, `http://127.0.0.1`, or HTTPS. WebGPU is unavailable from
`file://`.

Also check that the selected browser supports WebGPU and is not blocked by local
GPU policy.

### `No available adapters`

Use an installed Chrome or Edge browser instead of Playwright's downloaded
Chromium/headless shell. On Windows:

```powershell
$env:CHROME_PATH = "C:\Program Files (x86)\Google\Chrome\Application\chrome.exe"
node src_test/browser/run_webgpu_browser_test.mjs `
  build-webgpu-browser/src_test/LLGI_Test.html
```

The runner uses `--use-angle=d3d11`. Vulkan or SwiftShader flags can leave
`requestAdapter()` returning `null` on this machine.

### `A WebGPU-capable Chrome or Edge executable is required`

Set `CHROME_PATH` to the browser executable:

```powershell
$env:CHROME_PATH = "C:\Program Files (x86)\Google\Chrome\Application\chrome.exe"
```

### `TimedWaitAny not enabled at wgpuCreateInstance`

Rebuild `LLGI_Test` after changing WebGPU source files:

```bash
cmake --build build-webgpu-browser --target LLGI_Test
```

The browser path requires `wgpu::InstanceFeatureName::TimedWaitAny` when
creating the Emscripten WebGPU instance because readback waits use timeout-based
`Instance::WaitAny`.

### Emdawnwebgpu port build fails with a cross-drive path error

Put `EM_CACHE` on the same drive as emsdk:

```powershell
$env:EM_CACHE = "D:\emscripten-cache-llgi"
cmake --build build-webgpu-browser --target LLGI_Test
```

### Browser shows a dark or blank page

Most tests render offscreen and validate results through readback. Only
`WebGPUBrowser.ScreenPresentation` intentionally draws to the visible canvas.

Open:

```text
http://localhost:8000/LLGI_Test.html?filter=WebGPUBrowser.ScreenPresentation
```

### Logs show `LLGI_TEST_FAIL`

Check the preceding browser console lines. The test code prints `Abort on
<file> : <line>` for assertion failures, and the runner also reports WebGPU
validation errors captured by the browser.
