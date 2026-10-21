# Starter gameplay checks

Run from the repository root with the pinned .NET SDK:

```sh
dotnet run --project Tools/Tests/StarterGameplayTests/StarterGameplayTests.csproj -c Release
```

The dependency-free console runner compiles both actual template controllers and references the real HEngine.ScriptCore project. Its fake IEngineApi loads the authored scenes and input maps. Checks cover stable script IDs, diagonal movement, blocked movement, arena bounds, hit/nearest-hit/miss/occlusion, fire cooldown, timed target respawn, reset, aim/look controls, HUD updates and owned UI teardown.

Evidence type: unit-test. It does not create a renderer or validate native input, resource loading or images. Use the separate bounded Windows native validation for those integration paths. No third-party test framework or NuGet test package is required.
