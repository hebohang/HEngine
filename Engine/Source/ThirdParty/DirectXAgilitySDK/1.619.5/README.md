# DirectX 12 Agility SDK 1.619.5

This directory is an engine build/runtime dependency, not project content.
It contains the minimal Windows x64 subset used by HEngine so CMake configure,
reflection codegen, Editor and Game builds remain deterministic and offline.

Source package:
`Microsoft.Direct3D.D3D12.1.619.5.nupkg`

Package SHA-512:
`f7c0a819610066d7cf03d18a4349f51b4f1e9b4f1b3f8ff5f7fbd9490e683471696e6fe50a11f8639efe3247c16ae18799f30bd36096160797dd880a0fceb47f`

Bundled files:

- `Include/`: the package's MIT-licensed headers.
- `Runtime/x64/D3D12Core.dll`: the redistributable x64 runtime.
- `LICENSE.txt`, `LICENSE-CODE.txt`, `distributable files.txt` and the NuGet
  manifest: provenance and redistribution terms.

`CMake/DirectXAgilitySDK.cmake` validates the NuGet version, header SDK version
and the runtime SHA-256 before creating any build target.
