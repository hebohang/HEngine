---
name: he-vs-shared-build
description: Reuse a specific Visual Studio Open Folder CMake directory only when the user explicitly asks for VS handoff or shared build artifacts; never use it as the default Agent validation path.
---

# Explicit VS handoff

Determine the user's active `CMakeSettings.json` build root, then pass it explicitly:

```powershell
powershell -ExecutionPolicy Bypass -File Tools\Agent\Build-SharedEditor.ps1 -BuildRoot out\build\<active-config> -Target HEngineEditor
```

The script rejects a missing cache and a busy Ninja lock. Do not delete `.ninja_lock`, kill the user's VS, or silently switch to another build directory.

Report the active VS configuration, build directory, target, exact command, and `build-only` PASS/FAIL. Use `he-build-validate` for all work that does not require shared VS artifacts.
