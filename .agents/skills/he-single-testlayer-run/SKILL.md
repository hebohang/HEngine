---
name: he-single-testlayer-run
description: Run one HEngine logic or serialization test with the headless HEngineTests runner.
---

# Single engine test

List available tests only when the requested name is unknown:

```powershell
.\hectl.bat test list
```

Run exactly one named test:

```powershell
.\hectl.bat test PbrLightingReflection
```

The command delegates to `Run-Test.ps1`. Do not toggle test calls in source. The
runner builds incrementally in `out/agent/tests-debug`, starts no window, times
out safely, and uses exit codes for PASS/FAIL. Report the name, exact command,
result, and evidence type `unit-test`. Use Editor runtime validation only for
tests that truly require Window, Renderer, or UI interaction; third-party
`ctest` is not equivalent.
