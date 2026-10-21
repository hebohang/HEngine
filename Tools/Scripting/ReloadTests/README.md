# JSON reload regression

Run from the repository root. This fixture calls the real scene service serializer,
then verifies 12 collectible reloads, live-instance refusal and candidate preservation.
The original host fails on the first reload because shared JSON caches retain ScriptCore types.
The normal sample.touch run continues to cover cancellation and retained external roots.

```powershell
./Tools/Scripting/BuildManaged.ps1 -Project Tools/Scripting/ReloadTests/Fixtures -WorkDirectory out/agent/scripting/editor-ui-json-regression-work -OutputDirectory out/agent/scripting/editor-ui-json-regression-output -EditorHost
$api = (Resolve-Path out/agent/scripting/editor-ui-json-regression-work/api).Path
dotnet run --project Tools/Scripting/ReloadTests/ReloadTests.csproj -c Release --artifacts-path out/agent/scripting/editor-ui-reload-regression "-p:GeneratedApiDir=$api" -- out/agent/scripting/editor-ui-json-regression-output test.json-reload
```

Do not rebuild a fixture directory while its tests are running; use a separate output directory for concurrent work.
