---
name: he-reflection-codegen
description: Regenerate reflection outputs and compile the affected target after changes to reflection macros, reflected fields/types, components, or resources in HEngineDev.
---

# Reflection validation

Never edit generated files manually. `Engine/Source/_generated/` is a legacy
location and must not be used as build or validation evidence. Active outputs
live under the selected build's `GeneratedSource`; their authoritative state is
the selected output root's `.he-codegen/manifest.json`. Agent Game, Editor, and
Tests use `out/agent/game-<config>/GeneratedSource`,
`out/agent/editor-<config>/GeneratedSource`, and
`out/agent/tests-<config>/GeneratedSource`. Multi-config Package uses
`out/package/build/GeneratedSource/Release`.
Direct Tests-consumer CMake configurations must set both
`HE_BUILD_TESTS=ON` and `HE_CODEGEN_HOST_OVERRIDE=Tests`.

Use the compact interface from the repository root:

```powershell
.\hectl.bat codegen status
.\hectl.bat codegen check
.\hectl.bat codegen run
.\Win-CppCodeGen.bat --no-pause
```

`status` is read-only and reports dirty reasons. `check` is read-only and exits
nonzero when dirty. `run` publishes transactionally when needed. Add `--json`
for the stable machine schema or `--quiet` for silent success. The batch entry
remains compatible and targets `build/GeneratedSource`; normal Game, Editor,
Tests, and Package builds use their own host/config-specific roots through the
`HEngineCodeGen` CMake node. Full logs are under `out/agent/logs`.

After generation, use `he-build-validate` to compile the smallest affected
target (`HGame` by default; `HEngineEditor` for Editor or `WITH_EDITOR` code).
Do not reuse the VS shared cache. A final no-op check must report `changed=0`,
`deleted=0`, and no generated output hash/mtime changes; inspect the manifest
instead of scanning or diffing the entire generated tree.

Report codegen and build as separate commands with PASS/FAIL. If `he-resource-reflection-serialization` also triggers, follow its design constraints but do not repeat codegen.
