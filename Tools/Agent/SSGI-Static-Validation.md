# Current-editor SSGI ground stability, 2026-09-09

The user's live PBRTest editor view was captured before closing the editor:
position (−5.462834, 4.411538, 11.218872), transform rotation in radians
(−0.5475003, −1.5434462, 0), vertical FOV 60°, viewport 3102×1257.
`Project/Assets/RenderingBenchmarks/Validation/SSGILive/CurrentEditor.he`
preserves this camera and the saved light transform, 100,000 lx, EV100 12.
PBRTest currently loads the older Sponza asset; it is not the Intel benchmark.
The scene's IBLIntensity=1 was preserved for exact reproduction. Its shadow
floor is effectively black with SSGI off, as the control below confirms.

The GBuffer vertex shaders incorrectly took abs(view Z) before interpolating
previous view depth. Large ground triangles straddle the camera plane, so
interpolation of the absolute vertex values disagreed with actual pixel depth.
SSGI and TAA repeatedly rejected otherwise stationary ground history. Both the
material and built-in opaque vertex paths now interpolate signed forward depth.
The existing pixel visibility checks still reject invalid previous projections.

Experiments extending temporal history, skipping static clipping, and relaxing
normal rejection were removed: they did not materially improve this view.
The final fix does not add rays or filter passes. All original surface identity,
depth and normal rejection checks remain in place.

## Captured observations

Evidence directory: `out/agent/ssgi-live-20260909`. Metrics use the mean of
per-channel temporal standard deviation in display RGB code values 0..255,
over 8–12 static observations. Capture barriers are recorded; captures are not
claimed to be adjacent render frames. ROI coordinates are in the analyzer.

| Capture | Resolution | Shadow ground std | Sunlit ground std |
|---|---|---:|---:|
| baseline2, Low before fix | 1551×628 | 1.3861 | 0.6304 |
| fix-signed-depth, Low after fix | 1551×628 | 0.4196 | 0.3340 |
| off, control | 1551×628 | 0.000043 | 0.3862 |
| native-low | 3102×1257 | 0.2819 | 0.3814 |
| high | 1551×628 | 0.1963 | 0.3401 |

At matched Low settings, shadow ground mean deviation falls 69.7%; its 95th
percentile falls from 4.8898 to 0.7592. Sunlit ground mean deviation falls 47.0%.
The shadow region still contains positive GI (mean 0.6247 vs off 0.000027),
although it remains very dark. These results do not establish noise-free walls,
complete scene GI, moving-camera ghosting quality, or visibility outside the screen.

## Commands and status

All commands run from the repository root. `$p` below denotes the Python runtime
with Pillow and NumPy:
`C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe`.

```powershell
& Tools/Agent/Test-SSGIStaticSequence.ps1 -OutputDirectory out/agent/ssgi-live-20260909/fix-signed-depth -SettingsPath out/agent/ssgi-live-20260909/low.json
& Tools/Agent/Test-SSGIStaticSequence.ps1 -OutputDirectory out/agent/ssgi-live-20260909/native-low -SettingsPath out/agent/ssgi-live-20260909/low.json -Resolution 3102x1257
& Tools/Agent/Test-SSGIStaticSequence.ps1 -OutputDirectory out/agent/ssgi-live-20260909/high -SettingsPath out/agent/ssgi-live-20260909/high.json -Samples 8
& $p Tools/Agent/Analyze-SSGIStaticSequence.py out/agent/ssgi-live-20260909/baseline2 out/agent/ssgi-live-20260909/fix-signed-depth out/agent/ssgi-live-20260909/native-low out/agent/ssgi-live-20260909/high --output out/agent/ssgi-live-20260909/static-report.json
```

These are runtime-manual PASS for owned process/capture completion and the
reported numeric comparison. Use a new output directory for reruns. The script
always stops its own session in finally. The initial 240-frame step attempt
failed the API's 120-frame bound; the script now chunks warmup appropriately.

An additional `HE_SSGI_SMOKE=1; Tools/Agent/Build.ps1 -Target HEngineDX12SceneSmoke
-Configuration RelWithDebInfo -Run -Wait -TimeoutSeconds 180` attempt is FAIL:
the generic suite expects an obsolete 21-pass Forward graph and six bindless
textures, but current defaults produce a Deferred graph. It exits before the
SSGI subtest; no SSGI motion/resize coverage is claimed from that run.

The user editor was saved and gracefully closed at 04:52:34. A subsequent owned
editor check is separate from that user process and exits automatically.

Final runtime-manual PASS:

```powershell
hectl.bat game check -StartupScene Assets/RenderingBenchmarks/Validation/SSGILive/CurrentEditor.he -RenderPath Deferred -Resolution 1551x628 -Frames 360 -PerformanceWarmupFrames 120 -RenderStats -RenderStatsPath out/agent/ssgi-live-20260909/perf-off.csv -RenderSettingsPath out/agent/ssgi-live-20260909/off.json -TimeoutSeconds 180
hectl.bat game check -StartupScene Assets/RenderingBenchmarks/Validation/SSGILive/CurrentEditor.he -RenderPath Deferred -Resolution 1551x628 -Frames 360 -PerformanceWarmupFrames 120 -RenderStats -RenderStatsPath out/agent/ssgi-live-20260909/perf-low.csv -RenderSettingsPath out/agent/ssgi-live-20260909/low.json -TimeoutSeconds 180
hectl.bat editor check -StartupScene Assets/RenderingBenchmarks/Validation/SSGILive/CurrentEditor.he -RenderPath Deferred -Frames 12 -TimeoutSeconds 180
```

The first low-performance build attempt collided with a simultaneous codegen
verification and failed its codegen node. The serialized retry passed; see
`perf-low-retry.log`. Both performance runs were taken after closing the user
editor, with 238 retained GPU samples each after warmup. On the RTX 3060 Laptop,
mean GPU frame span is 1.925 ms with GI off and 3.582 ms with Low SSGI, a paired
increase of 1.656 ms at 1551×628. This is a short capture, not a native-resolution
or thermal-stability performance guarantee. `performance.json` and raw CSVs
retain the measurements. Final owned editor check also passed and exited.
