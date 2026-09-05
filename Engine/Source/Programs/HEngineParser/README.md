## Copyright Notice

Part of the codes is derived from 
[CPP-Reflection](https://github.com/AustinBrunkHorst/CPP-Reflection) Copyright (c) 2016 Austin BrunkHorst
[Piccolo](https://github.com/BoomingTech/Piccolo) Copyright (c) 2022 Contributors to the Piccolo Engine

## HEngine codegen workflow

`HEngineParser` is the parse/IR/render stage behind the `HEngineCodeGen` CMake
target. It parses with the real host/config include and define context, writes
only to a sibling staging root, and emits `.he-codegen-index.json`. The driver
validates that index and transactionally publishes write-if-changed outputs plus
`.he-codegen/manifest.json`.

Use `hectl codegen status|check|run`; add `--json` or `--quiet` for Agent use.
The compatibility `Win-CppCodeGen.bat --no-pause` entry remains supported.
Logs are stored under `out/agent/logs`. Do not inspect or edit
`Engine/Source/_generated`; active output is always the selected build's
`GeneratedSource` root.

Windows parsing requires libclang 20 or newer because the current MSVC STL and
engine headers use modern C++20 syntax. Set `HE_CODEGEN_LIBCLANG_ROOT` (and, for
a complete LLVM installation, `HE_CODEGEN_LIBCLANG_INCLUDE_ROOT`) to override
automatic Visual Studio LLVM discovery.
