<p align="right"><a href="README.md">简体中文</a> · <strong>English</strong></p>

<p align="center">
  <img src="Engine/Resources/Icons/EngineIcon.png" alt="HEngine Logo" width="128" />
</p>

<h1 align="center">HEngine</h1>

<p align="center">A modern C++20 game engine with visual authoring, C# gameplay, and Windows, macOS, Android, Web, and WeChat Mini Game workflows.</p>

<p align="center">
  <a href="RELEASE_NOTES.md"><img alt="Version 1.1.0" src="https://img.shields.io/badge/version-1.1.0-4c8bf5" /></a>
  <img alt="Windows, macOS, Android, Web, WeChat" src="https://img.shields.io/badge/platforms-Windows%20%7C%20macOS%20%7C%20Android%20%7C%20Web%20%7C%20WeChat-0078d4" />
  <img alt="C++20" src="https://img.shields.io/badge/C%2B%2B-20-00599c" />
  <img alt="Renderer DX12 and Vulkan 1.3" src="https://img.shields.io/badge/renderer-DX12%20%7C%20Vulkan%201.3-6f42c1" />
  <a href="LICENSE"><img alt="MIT License" src="https://img.shields.io/badge/license-MIT-blue" /></a>
</p>

<p align="center">
  <a href="#core-features">Core Features</a> ·
  <a href="#quick-start">Quick Start</a> ·
  <a href="#samples-and-templates">Samples</a> ·
  <a href="#documentation">Documentation</a> ·
  <a href="RELEASE_NOTES.md">1.1.0 Release Notes</a>
</p>

<p align="center">
  <img src="Docs/Images/README/material-showcase.png" alt="HEngine material showcase scene" width="960" />
</p>

HEngine is an open-source C++20 game engine for scene authoring, asset import, C# / Python scripting, native C++ extensions, rendering, physics, and game publishing.

**1.1.0** provides a standalone Windows x64 Editor SDK, Project Manager, and precompiled player templates. The engine defaults to the Blank template; [Starbloom Survivor](https://github.com/hebohang/StarbloomSurvivor) is maintained as a separate game project.

**[Download HEngine 1.1.0](https://github.com/hebohang/HEngine/releases/tag/v1.1.0)** · [Platform support](#support-matrix) · [Build and publish](Docs/BuildAndPublish.md)

## Core Features

- **Visual editor**: Scene hierarchy, inspector, content browser, asset previews, and dedicated material, animation-controller, and runtime-UI tools.
- **Modern rendering architecture**: `RenderWorld -> SceneRenderer -> RenderGraph -> NVRHI`, with DirectX 12 and Vulkan 1.3 supported by the same runtime.
- **Materials and animation**: PBR materials, custom Surface Shaders, material hot reload, skeletal animation, and animation state machines.
- **3D physics**: Jolt Physics integration with rigid bodies, common collider shapes, character movement, ray/shape queries, and editor debug drawing.
- **Scripting and gameplay**: C# gameplay APIs, Python editor gameplay, native C++ extensions, reflection, serialization, and AI development documentation. Cross-platform exports primarily use C#.
- **Asset and content pipeline**: Scene, Prefab, Mesh, Material, Texture, Audio, and UI resources, with glTF import and project-level content overrides.
- **UI, effects, and audio**: RmlUi, Effekseer, and the engine audio abstraction integrated with the runtime lifecycle.
- **Build and release workflow**: `hectl` provides isolated Game, Editor, test, capture, and Package flows; the Cooker validates dependency closure, manifests, hashes, and `Game.pak`.

## Quick Start

### Precompiled SDK (Windows x64)

1. Download and extract `HEngine-1.1.0-Windows-x64.zip` from the [1.1.0 release](https://github.com/hebohang/HEngine/releases/tag/v1.1.0).
2. Run `ProjectManager.exe` to create a project or import an existing `Project.heproject`.
3. Press **F6** to play and **F8** to stop. Open **Build → Build and Publish** to select an export target.

Precompiled samples do not require Visual Studio or a system .NET installation. Editing C# and exporting games still require the documented tools. Windows / Android exports reuse precompiled native templates; Web / WeChat require the Wasm toolchain. Check SDK dependencies with `hectl deps check -Profile SDK`; see [SDK export capabilities](Docs/ReleaseSDK-1.1.0.md).

### Build from source (Windows)

Install Visual Studio / Build Tools 2026 with desktop C++ support (v145; VS2022 v143 is also supported) and the Windows SDK. The build uses C++20 and CMake 4.2+.

```powershell
git clone https://github.com/hebohang/HEngine.git
Set-Location HEngine
.\Win-InstallDependencies.bat -NoPause
.\hectl.bat editor check -Project Engine/Templates/Blank
```

The dependency script prepares pinned tools but does not install Visual Studio. `editor check` builds and performs a bounded startup check. Use `Win-RunEditor.bat` for interactive work and `-Project D:/Games/MyGame` for an external project. The [macOS source workflow](Tools/macOS/README.md) has its own toolchain and entry points.

### Common commands

| Command | Purpose |
| --- | --- |
| `.\hectl.bat project current -Json` | Show the selected project and output paths |
| `.\hectl.bat editor check` | Build and run a bounded Editor startup check |
| `.\hectl.bat game check` | Build and run a bounded Game check |
| `.\hectl.bat publish check -BuildProfile web` | Check project Web export settings and dependencies |
| `.\hectl.bat publish build -BuildProfile web` | Export the project's Web profile |

Reflection code generation runs through the build graph. Native C++ extensions require source builds and matching templates; no stable binary ABI is promised across releases.

## Samples and Templates

- [Blank](Engine/Templates/Blank): the default empty project.
- [SharedArena](Engine/Templates/SharedArena): shared C# rules targeting Windows, Android, Web, and WeChat.
- [Starbloom Survivor](https://github.com/hebohang/StarbloomSurvivor): a separate game; `main` contains the C# four-target project and `python` contains Python editor gameplay.
- This source distribution includes the complete engine, developer tools, and project templates. The large `RenderingTests` project and datasets remain in the development repository and are excluded from this lean public snapshot.

Project Manager supports creation, import, and recent projects. See [project conventions](Docs/Projects.md).

## Support Matrix

Platform workflows and downloadable binaries are distinct. Graphics features and tooling requirements vary by platform.

| Platform | Workflow / graphics | 1.1.0 delivery and scope |
| --- | --- | --- |
| **Windows x64** | Editor, game development, Shipping export; DX12 / Vulkan 1.3 | Precompiled Editor SDK, Windows players, and Project Manager; the main binary validation platform |
| **macOS (Apple Silicon / ARM64)** | Native Editor, Game, and `.app` packaging; Vulkan → MoltenVK → Metal | Source workflow with M4 Pro editing and gameplay validation; no Mac SDK download in this release; distribution requires your signing and notarization |
| **Android ARM64** | C# games, native Vulkan 1.3, APK export | Precompiled native template in the Windows SDK; requires Android tools and project signing; consult the SDK and device validation records |
| **Web** | C# / .NET Wasm, WebGL 2, browser games | Export tools included; requires Wasm tools, project adaptation, and HTTP hosting |
| **WeChat Mini Games (weixin)** | C# / .NET Wasm, WeChat WebGL 2, resource subpackages | Export tools included; requires your AppID, WeChat DevTools, and device testing before launch |

Web / WeChat use portable rendering and host adapters, without full native rendering, physics, or plugin parity. SharedArena shares gameplay rules; arbitrary games still need adaptation. Historical Android / WeChat device checks do not replace validation of a final exported project.

**Windows ARM64** build and validation infrastructure is present, but this release has no native Windows ARM64 SDK attachment. Android and macOS ARM64 support does not imply Windows ARM64 support. Linux and iOS are outside the current release scope.

Windows Game / Editor include DX12 and Vulkan, selected with `--graphics-api=dx12|vulkan` (DX12 by default). See [Windows graphics compatibility](Docs/WindowsGraphicsCompatibility.md) and [build/export profiles](Docs/BuildAndPublish.md).

## Documentation

| Topic | Document |
| --- | --- |
| 1.1.0 contents and release boundary | [RELEASE_NOTES.md](RELEASE_NOTES.md) |
| Standalone SDK and four-target exports | [Docs/ReleaseSDK-1.1.0.md](Docs/ReleaseSDK-1.1.0.md) |
| macOS development and packaging | [Tools/macOS/README.md](Tools/macOS/README.md) |
| Android tooling and signing | [Docs/Android.md](Docs/Android.md) |
| Web / WeChat and shared content | [Docs/BuildAndPublish.md](Docs/BuildAndPublish.md) |
| Scripting and AI development | [Scripting guide](Docs/ScriptingGuide.md) · [AI development](Docs/AI_GAME_DEVELOPMENT.md) |
| Graphics backends and hardware capabilities | [Docs/WindowsGraphicsCompatibility.md](Docs/WindowsGraphicsCompatibility.md) |
| Multi-project and content boundaries | [Docs/Projects.md](Docs/Projects.md) |
| glTF / Mesh import | [Docs/AssetImporting.md](Docs/AssetImporting.md) |
| Shader-driven materials | [Docs/ShaderMaterials.md](Docs/ShaderMaterials.md) |
| Runtime UI motion | [Docs/RuntimeUiMotion.md](Docs/RuntimeUiMotion.md) |
| Rendering architecture | [Engine/Source/Runtime/Graphics/RenderingArchitecture.md](Engine/Source/Runtime/Graphics/RenderingArchitecture.md) |
| GM developer tools | [Docs/GM.md](Docs/GM.md) |

## Repository Layout

```text
Engine/   Engine source, built-in content, and tooling resources
Examples/ Lightweight scripting samples; the default is Engine/Templates/Blank
Docs/     Feature and workflow documentation
Tools/    Build, validation, and content-processing tools
```

## Contributing

Issues and pull requests are welcome. Keep changes focused, avoid unrelated large-scale refactors, and run the smallest build or test that covers the change before submitting it. See [AGENTS.md](AGENTS.md) for repository development conventions.

## License and Acknowledgements

HEngine is available under the [MIT License](LICENSE). Third-party code and assets retain their respective licenses, which are distributed with the corresponding components or release package.

HEngine has been inspired and helped by these projects and learning resources: [Hazel](https://github.com/TheCherno/Hazel), [Pilot](https://github.com/BoomingTech/Pilot), [Godot](https://github.com/godotengine/godot), [LearnOpenGL](https://github.com/JoeyDeVries/LearnOpenGL), [LearningDirectX12](https://github.com/jpvanoosten/LearningDirectX12), [DirectX11-With-Windows-SDK](https://github.com/MKXJun/DirectX11-With-Windows-SDK), [Adria](https://github.com/mateeeeeee/Adria-DX12), [Vulkan Guide](https://github.com/vblanco20-1/vulkan-guide), [MoravaEngine](https://github.com/dtrajko/MoravaEngine), and [Ogre](https://github.com/OGRECave/ogre).
