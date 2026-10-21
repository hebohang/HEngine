# Native-resolution HUD and allocation rollback

`Tools/Agent/Test-FsrHud.ps1` exercises the real SickaDynasty gameplay HUD while
simulation is paused. It compares native rendering with FSR Performance at1440p
and2160p, records HUD rectangles and executing render-graph passes, and verifies
the actual upscaler and TAA state. It takes an explicit project and binary hash;
project paths and sessions are resolved by hectl.

```powershell
$hash=(Get-FileHash out/agent/projects/7e44ffe2e91e72bd/game-relwithdebinfo/bin/Gamee4fb9eeb.exe).Hash
powershell -ExecutionPolicy Bypass -File Tools/Agent/Test-FsrHud.ps1 -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab -ExistingBuildSha256 $hash -GraphicsApi DX12 -OutputDirectory out/agent/captures/fsr-hud-DX12-verified-20260929
powershell -ExecutionPolicy Bypass -File Tools/Agent/Test-FsrHud.ps1 -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab -ExistingBuildSha256 $hash -GraphicsApi Vulkan -AllowNative4KRejection -OutputDirectory out/agent/captures/fsr-hud-Vulkan-verified-20260929
```

The optional rejection branch does not force an allocation failure. If native4K
is rejected, it requires unchanged settings revision/hash, renders through more
than all three frame banks and checks that the original FSR view and HUD survive.
If the transition succeeds, it captures the real native4K view instead. Success
is written only after owned-process shutdown, with runtime/shutdown validation
errors and nonempty stderr rejected. Rejection does not count as a native4K image
comparison or prove that a failure occurred when it did not.

Analyze with the configured Python runtime containing NumPy and Pillow:

```powershell
C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe Tools/Agent/analyze_fsr_hud.py out/agent/captures/fsr-hud-DX12-verified-20260929 out/agent/captures/fsr-hud-Vulkan-verified-20260929
```

The analyzer verifies pause control, completed simulation steps and actual elapsed
simulation time. `applicationSimulationFrame` increments on zero-delta rendering
frames too, so its equality is not a valid pause test. It checks native raster
dimensions, exact HUD layout, opaque glyph/stroke interiors and the executing
FSR -> HDR post -> tone map -> native UI -> present order. Background-dependent
antialiased edges are excluded. `native2160Compared` and
`rejectionRollbackVerified` distinguish the two4K outcomes. Generated previews
are at most1280px; review them before claiming visual acceptance.

Current-source results and historical failure evidence are recorded in
[FSRStreamingCombatWork.md](FSRStreamingCombatWork.md). Passing a normal transition
does not prove out-of-memory recovery or resolve the pending third-party cleanup
defect.
