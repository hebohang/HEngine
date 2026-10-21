---
name: he-reflection-codegen
description: Validate HEngine reflection declarations, persisted resource fields, or code generation changes with generation and compilation.
---

# Reflection and resource serialization

- Declare persisted resource types/fields with the repository reflection macros;
  generated serializers remain the source of truth.
- Do not add field-specific patches to generic paths such as
  `SceneSerializer::SerializeEntity` or `DeserializeEntity`.
- Never edit generated files manually. `Engine/Source/_generated/` is legacy;
  it and stale outputs without a manifest are not build or validation evidence.

## Generate and compile once

Build the smallest affected consumer through `he-build-validate` (or the test/package
route when that is the task). Its `HEngineCodeGen` node ensures generation before
compilation. Reuse this result; ordinary reflected-field changes do not require a
separate generation command or an extra runtime run solely because this skill applies.
Report codegen and build outcomes separately, citing the same command when appropriate.

Active outputs live in the selected build's `GeneratedSource`; authoritative state
is its `.he-codegen/manifest.json`. Game/Editor/Tests use
`out/agent/<game|editor|tests>-<config>/GeneratedSource`; multi-config Package uses
`out/package/build/GeneratedSource/Release`. Full logs are in `out/agent/logs`.

## Standalone diagnostics, only when needed

Use `hectl codegen status|check|run` with the consumer's explicit host/config:

| Consumer | Arguments |
| --- | --- |
| Default Game | `-CodeGenHost DevGame -Configuration RelWithDebInfo` |
| Default Editor | `-CodeGenHost Editor -Configuration RelWithDebInfo` |
| Tests | `-CodeGenHost Tests -Configuration Debug` |
| Package | `-CodeGenHost Package -Configuration Release` |

`status` reports dirty reasons; `check` is read-only and fails when dirty; `run`
publishes only changed content. Choose one action and use `--json` for machine
output or `--quiet` for silent success. Override configurations to match the actual build.
Do not rely on the standalone default (Editor/Debug) for a different consumer.
`Win-CppCodeGen.bat` targets the compatibility `build/GeneratedSource` only;
do not append it to a normal build. `CPP_CODE_GEN` is a cache compatibility flag,
not a configure-time generation trigger. Direct Tests CMake configurations need
`HE_BUILD_TESTS=ON` and `HE_CODEGEN_HOST_OVERRIDE=Tests`.

Only codegen driver/template/cache/publication changes or an explicit idempotence
request require an additional no-op verification. Then report `changed=0`,
`deleted=0`, and actual before/after output hash/mtime equality. A clean manifest
alone cannot prove unchanged mtime; keep snapshots on disk and report a bounded summary.
