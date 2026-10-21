---
name: he-build-validate
description: Build or run the smallest affected HEngine Game, Editor, parser, or shader compiler target using hectl.
---

# Build and validate

Run from the engine workspace or a project directory. Inspect `hectl project current -Json`
for the selected project and output paths; the checked-in workspace defaults to `Engine/Templates/Blank`.
`hectl project use -Project <path>` persists a local workspace default, and `-Project`
overrides it for one invocation. External projects use short hashed subdirectories
under the engine's `out/agent/projects`, not the default game's binaries.
`hectl` uses Ninja and the VS compiler toolchain
without starting VS IDE. Game/Editor default to `RelWithDebInfo`, tests to `Debug`;
caches stay in `out/agent`, with logs in `out/agent/logs`.
The zero-argument `hectl` builds HGame with `HE_BUILD_DEV_GAME=ON`, `PACK_GAME=OFF`.

## Choose the required evidence

Select the affected target and one suitable route. Editor-only changes do not
require Game validation. `check` includes building and a bounded run; `capture`
also includes a screenshot. Do not run all three merely because they are listed.

| Purpose | Game | Editor |
| --- | --- | --- |
| Compile only | `hectl game build` | `hectl editor build` |
| Runtime integration | `hectl game check` | `hectl editor check` |
| Inspect visual behavior | `hectl game capture -Frames 30` | `hectl editor capture -Frames 30` |

Choose frame counts and inputs that exercise the requested behavior; a startup
capture alone does not prove an interaction works. Reuse sufficient validation
already performed for the same changes. For logic tests, use `he-single-testlayer-run`;
for reflected declarations or resource fields, use `he-reflection-codegen`;
for shipping artifacts, use `he-pack-game`.

For parser or shader compiler changes, build only that target with
`powershell -ExecutionPolicy Bypass -File Tools/Agent/Build.ps1 -Target HEngineParser`
or `-Target HShaderCompiler`.

## Inspect and hand back the result

For interaction beyond a bounded run, use one owned `hectl agent start` session,
discover only the needed operations with `hectl agent discover`, and call
`hectl agent stop` in `finally`. Session step/capture calls wait for completion.
Game captures default to 1280x720 and Editor to 1920x1080; use
`-Resolution WIDTHxHEIGHT` when the observation needs another size.

The user's manual launchers `Win-RunEditor.bat` and `Win-RunGame.bat` build these
same default artifacts before leaving the application open. The user can also
run an existing `out/agent/editor-relwithdebinfo/bin/HEngineEditor.exe` or
`out/agent/game-relwithdebinfo/bin/StarbloomSurvivor.exe` directly, without rebuilding.
Agent validation uses owned routes, not these manual launchers or `open`.
If a user process blocks replacing its executable, report that concrete blocker;
do not kill the process or poll indefinitely. Do not switch to `out/build`.

Report the exact command, `build-only` or `runtime-manual`, and PASS/FAIL.
