# Continuous texture streaming validation

`Tools/Agent/Test-StreamingSoak.ps1` runs an owned Game session through normal,
reduced and restored texture budgets while the camera continues moving. It uses
the reusable engine streaming/settings APIs; the scene fixture belongs to the
external MechanicGirlLab project.

## Fixture and run

In MechanicGirlLab, run `Tools/Build-StreamingTraversal.py` to derive
`Assets/Scenes/Diagnostics/StreamingTraversal.he` from SickaDynasty. This preserves
the environment, lights and animated character while removing scene scripts and
sequences. `Scripts/CSharp/StreamingTraversalDiagnostic.cs` drives the primary
camera around an elevated ellipse every ten simulation seconds and requests
VSync. This is an aggressive residency diagnostic, not a playable route or a
collision-clearance-certified benchmark. Gameplay and combat are tested separately.

Build the external Game once, then pin that binary for each backend:

```powershell
powershell -ExecutionPolicy Bypass -File Tools/Agent/hectl.ps1 game build -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab
$hash = (Get-FileHash out/agent/projects/7e44ffe2e91e72bd/game-relwithdebinfo/bin/Gamee4fb9eeb.exe).Hash
powershell -ExecutionPolicy Bypass -File Tools/Agent/Test-StreamingSoak.ps1 -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab -GraphicsApi DX12 -ExistingBuildSha256 $hash -OutputDirectory out/agent/captures/streaming-soak-DX12
powershell -ExecutionPolicy Bypass -File Tools/Agent/Test-StreamingSoak.ps1 -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab -GraphicsApi Vulkan -ExistingBuildSha256 $hash -OutputDirectory out/agent/captures/streaming-soak-Vulkan
```

Each output directory must be new. The driver resolves the project without
changing the default workspace. FSR 3.1 Quality reconstructs 853x480 to 1280x720;
GI is Off. Native GPU validation is disabled for timing. The warmup phase spans
at least 1200 frames, followed by at least 3600 frames each at 512, 128 and 512 MiB.
System counter polling can overshoot the requested frame count. The simulation
delta is fixed at 1/60 s, and each phase has a ten-minute wall-clock deadline.

The driver samples the owned process's WDDM GPU Process Memory counters every
few seconds, checks its path and creation time, and captures one image and raster
metadata snapshot at each phase endpoint. A `finally` block stops only that
owned session and preserves logs even when Stop fails. Successful collection
requires a verified zero process exit; merely capturing all four phases is
insufficient. Performance sessions have a bounded 60-second graceful shutdown
allowance because buffered statistics are formatted and written after rendering.
Other Agent sessions retain their five-second allowance.

For a targeted NVIDIA slowdown investigation, add `-NvidiaTelemetry`. This
requires the installed driver's `nvidia-smi` and starts an owned, hidden,
read-only monitor at 500 ms intervals. `gpu-telemetry.csv` records GPU UUID,
P-state, graphics/memory clocks, utilization, power, temperature and raw clock
reason masks. The driver stops that exact monitor after Game exits, including
failure paths. It never changes clocks, power limits or driver settings. Local
timestamp offset is recorded for matching samples to UTC CPU frame timestamps.
Hardware samples cover the entire GPU, whereas WDDM samples cover the Game PID.
Nearest samples in the analysis establish correlation only; sub-sample stalls,
preemption and CPU scheduling require additional evidence for attribution.

## Analysis and limits

Run `Tools/Agent/analyze_streaming_soak.py <output-directory> ...` with Python,
NumPy and Pillow. It requires successful shutdown and the native statistics
completion marker. It checks capture capacity/completeness, FSR active with
standalone TAA disabled, planned texture payload within budget, zero failed
uploads, camera route endpoints and budget pressure/recovery. It writes
`analysis.json` and a compact `soak-preview.jpg` for subsequent visual review.
It reports endpoint bytes above the target and flags values above target+64MiB.
The latter is an observation, not a hard-cap assertion: the production demotion
admission rule uses `max(budget, current residency)+64MiB`, whereas promotion
admission uses `budget+64MiB`. Concurrent old/new resources and retirement can
therefore exceed target+64MiB. The driver does not certify a global scratch cap.

Timing excludes the first 120 frames of each phase and the endpoint capture.
The report includes CPU wall, resource pump, preparation and Present times,
per-frame GPU timing observations, and deduplicated GPU submission timing samples.
Structural checks do not assert a universal frame-rate or hitch threshold. VSync,
fixed simulation time, logging, counter polling and diagnostic buffers make this
unsuitable for unthrottled API performance comparisons.

Optional `-CpuTelemetry` adds an owned, hidden, read-only Windows PDH monitor at
one-second intervals. It records per-logical-processor and total Actual Frequency,
Processor Frequency, % Processor Performance, % Processor Utility and Performance
Limit Flags in `cpu-telemetry.jsonl`. `Monitor-CpuTelemetry.ps1` is bounded to3000
samples and stopped after Game shutdown by its owning driver. It changes neither
power policy nor affinity. Combine with `-NvidiaTelemetry` to investigate CPU and
GPU clock changes in the same run. Counter timestamps describe sampled intervals;
nearest-frame correlations do not prove causality. Counter scope is machine-wide,
not the Game process. The analyzer preserves per-processor raw samples near slow
frames, reports total-counter phase distributions and rejects missing coverage or
monitor errors. A partial final write after Game exit may be ignored; malformed
earlier records are errors.

Three memory quantities are deliberately distinct:

- Texture-manager payload and reserved replacements: a software residency budget,
  including its documented temporary scratch allowance.
- WDDM Dedicated/Shared/Total Committed counters for the exact Game PID, preserving
  each adapter instance. These include more resources than material textures.
- Native API memory-budget queries: the CSV's `processScope` explicitly determines
  whether the reported usage is per-process or device-heap-wide. The current
  Vulkan backend inherits the unavailable query; the analyzer reports this
  explicitly and uses WDDM for process-memory evidence instead of treating the
  unavailable native fields as zero usage. DX12 provides a process-scoped query.

The process's private memory also includes the preallocated CPU statistics buffers
and their committed pages. No single counter proves a physical-memory leak or
its absence. See Microsoft's [WDDM counter explanation](https://devblogs.microsoft.com/directx/gpus-in-the-task-manager/)
and [documented Windows 10 counter limitation](https://learn.microsoft.com/en-us/troubleshoot/windows-client/performance/gpu-process-memory-counters-report-wrong-value).
This diagnostic does not validate actual driver OOM/device-loss recovery,
multi-view global budgets, unlimited traversal, or an offline decoded mip cache
for ordinary image formats. Refer to `FSRStreamingCombatWork.md` for actual runs
and their qualified results.
