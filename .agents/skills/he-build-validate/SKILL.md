---
name: he-build-validate
description: Build and optionally run the smallest HEngine target in an isolated command-line Ninja directory. Use for HGame development runs, HEngineEditor checks, parser/compiler targets, and normal build validation that must not share Visual Studio state.
---

# Build and validate

Run from the repository root. The zero-argument daily default is the loose-project
HGame `RelWithDebInfo` development build (`HE_BUILD_DEV_GAME=ON`, `PACK_GAME=OFF`).
Game, Editor, and Agent routes default to `RelWithDebInfo`; tests retain `Debug`:

```powershell
.\hectl.bat
```

Use the owned check route after game changes. It exits deterministically and
closes the process even on failure:

```powershell
.\hectl.bat game check
.\hectl.bat game capture -Frames 30
```

Editor changes require an additional, explicit target. Its bounded check route
uses the same application exit protocol, owns the process, and closes it:

```powershell
.\hectl.bat editor build
.\hectl.bat editor check
.\hectl.bat editor capture -Frames 30
```

Game/session automation defaults to 1280x720. Editor automation defaults to
1920x1080 for a useful dock layout; override with `-Resolution WIDTHxHEIGHT`.

For live inspection without the editor, use one paused Agent-owned session.
Discover the small runtime surface instead of loading schemas eagerly, then
always stop the session in `finally`:

```powershell
.\hectl.bat agent start
.\hectl.bat agent discover
.\hectl.bat agent world
.\hectl.bat agent keys
.\hectl.bat agent input -AgentHeldKeys w
.\hectl.bat agent step -AgentStepFrames 1
.\hectl.bat agent clear-input
.\hectl.bat agent click -AgentClickX 640 -AgentClickY 520
.\hectl.bat agent capture -AgentCaptureName observed.png
.\hectl.bat agent stop
```

The session exposes bounded JSON state/log calls, deterministic pause/step,
isolated virtual keyboard input, bounded virtual clicks, and jailed live PNG
capture. Step and capture wait for completion barriers before returning.
`agent call -Method ... -ParamsJson ...` is the generic extension point.

Never use `game open` or `editor open` in Agent automation: they deliberately
leave a user-owned process running. Users may double-click `Win-RunGame.bat` or
`Win-RunEditor.bat`; built executables have stable paths under
`out/agent/<game|editor>-<config>/bin`. Manual open routes stream configure and
build output to the console while retaining the same files under `out/agent/logs`.

Use the lower-level script only for advanced targets or non-default bounds:

```powershell
powershell -ExecutionPolicy Bypass -File Tools\Agent\Build.ps1 -Target HEngineParser
powershell -ExecutionPolicy Bypass -File Tools\Agent\Build.ps1 -Target HShaderCompiler
powershell -ExecutionPolicy Bypass -File Tools\Agent\Build.ps1 -Run -ExitAfterFrames 120 -FixedDeltaSeconds 0.0166667 -NoCursorCapture
```

`hectl` delegates to `Build.ps1`; both use the VS2022 v143 toolchain, Ninja,
isolated `out/agent` caches, and `out/agent/logs`. Do not substitute a VS Open
Folder directory.

Report `build-only` or `runtime-manual`, the exact command, and PASS/FAIL. For reflection, package, unit tests, or explicit VS handoff, load the dedicated skill instead.
