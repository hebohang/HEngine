# Game internal render resolution

## FSR 3.1 reconstruction

The reusable main Game view supports AMD FSR 3.1.4 on DX12 and Vulkan through
`Rendering.Upscaling.Mode = "FSR3.1"` (`"Off"` selects the legacy path below).
`Rendering.Upscaling.Quality` accepts NativeAA, Quality, Balanced, Performance and
UltraPerformance; their output-to-input scale ratios are 1, 1.5, 1.7, 2 and 3.
Internal dimensions are floored to integers. `Rendering.Upscaling.Sharpness` is
0..1, default 0.2. These are RenderSettings service fields; current validation and
remaining acceptance work are tracked in `FSRStreamingCombatWork.md`.

Following the 2026-09-29 upstream NVRHI update, DX12 is the user's priority path.
Both APIs passed normal 1440p/2160p FSR/native switching with the new dependency;
Vulkan allocation-failure cleanup remains a known deferred issue. See
`NvrhiUpstreamValidation.md` for the exact revision, commands and limits.

Managed game menus can use `RenderPreferences.UpscalingModeId`,
`UpscalingQualityId` and `UpscalingSharpnessId` with `RenderPreferences.Apply`.
The service applies and saves the user layer through the same validated settings
transaction. `RenderPreferences.ReadDisplay()` returns the last recorded
`InternalWidth/Height`, `OutputWidth/Height`, `UpscalingActive`, `UpscalingFailed`
and `UpscalingFailureReason`. `UpscalingStatus` distinguishes `Off`, `Pending`,
`UnsupportedView`, `Suspended` (no valid output extent), `Active` and `Failed`.
Use the status for menu text: inactive without failure does not necessarily mean
that a setting is still being applied. The same status appears as `status` in
per-frame upscaling diagnostics. These diagnostics may lag one frame after a change;
do not infer actual activation solely from the requested setting. The failure flag
reports a provider failure, not view eligibility (editor/offline/debug views can
be ineligible without a provider error).

MechanicGirlLab exposes localized Off/FSR, all five quality levels and 5% sharpness
steps in its scrollable settings page. The legacy height target is hidden while
FSR is requested. The page reports actual dimensions and provider fallback state.
`SceneServices.ScrollIntoView` reveals a keyboard-selected Rml element without
changing focus or synthesizing activation, allowing menus to keep their own input
routing. Scrollable Rml documents must author scrollbar widths; unstyled vertical
scrollbars can otherwise consume the content width.

FSR takes precedence over the height cap while active and replaces standalone
TAA/spatial AA. Geometry, transparent objects and particles use the jittered
internal extent. FSR reconstructs scene-linear HDR to output size before HDR post
processing, tone mapping and native-size scene/window UI. Input HDR is not
pre-exposed (`preExposure=1`); FSR's internal luminance exposure is separate from
the subsequent artistic/automatic exposure pass. This integration provides
upscaling/NativeAA, not frame generation.

Mode/quality/output-size changes recreate the context and reset history. Camera
cuts, material publication and aborted frames also invalidate history; sharpness
alone preserves it. Missing providers select the configured legacy AA path. A
recoverable per-frame jitter/dispatch failure writes a spatially reconstructed
frame, then latches fallback to native AA. An explicit mode/quality change retries
FSR. Diagnostics retain the failure reason. GPU device loss is a separate host
failure and is not covered by this fallback claim.

FSR is currently eligible for the main swap-chain Game composite view. Editor,
external offscreen and offline-reference views retain their existing behavior.
When FSR is active, linear HDR capture is output-sized; native AOV captures retain
their source texture dimensions. `upscale-motion`, `upscale-color`,
`upscale-opaque`, `reactive` and `composition` provide the actual inputs as `.raw`.
The existing `motion` AOV remains the four-channel geometry/previous-depth guide.

## Legacy height cap and AA

`Rendering.Resolution.RenderHeight` is a persisted RenderSettings integer height cap.
`0` means native; positive values preserve the output aspect ratio and never exceed
native size. For example 3840x2160 output + 1080 produces 1920x1080 scene buffers;
3440x1440 + 1080 produces 2580x1080. MechanicGirlLab defaults to 1080 independently
of the quality preset. Existing projects default to native.

The main Game swap-chain path renders geometry, lighting, shadows' screen-space
consumers, effects, HDR post processing and TAA at the internal extent. The existing
HDR tone-map pass performs a clamped nine-bilinear-fetch Catmull-Rom reconstruction
before exposure/ACES/sRGB conversion, with no additional full-screen intermediate
for scaling. UI and final captures use the output extent. FXAA/SMAA are display-size
post filters; they do not provide the temporal stability of TAA. This is spatial
upscaling after TAA, **not** TSR, FSR, DLSS or frame generation. Fine hair and distant
details cannot recover all information of a native-4K image.

TAA accumulation history stays on the jittered raster grid. The following
`SceneTAAReconstruct` pass samples it at the current positive raster jitter offset
to produce an unjittered presentation copy before transparent overlays and tone
mapping. Without this step, internal subpixel jitter is magnified at 4K output.
The copy does not feed back into history. Depth/surface guides retain their own
jitter convention; consumers must continue using the feature-context depth flags.
This legacy correction is still not display-resolution temporal super resolution.

Authored mesh LOD error is evaluated in **output** pixels so a low internal render
height does not loosen the configured visible error limit. MechanicGirlLab's
reproducible static derivatives are produced with `Tools/Assets/Generate-SceneLods.mjs`
and consumed by its scene-authoring script after original lightmap binding.

Editor views, externally consumed offscreen outputs and offline-reference rendering
keep their native extent to preserve picking/depth/plugin contracts. Display-stage
extensions receive native Width/Height but depth guides retain the internal scene
extent; sample depth using normalized UVs and query the resource extent for integer
loads. Linear HDR diagnostic capture intentionally records the internal HDR image.

A cap or output-size change rebuilds the graph at the next frame boundary and resets
size-dependent temporal history. Configuration preparation resolves the candidate
extent from the physical output texture, and captures both internal/output extents
for transaction rollback. TAA resize retires old bindings/history but leaves final
allocation to PrepareFrame to avoid transient native-size allocations.

`SceneRendererStats` and render-stat CSV report both actual extents: `width/height`
are output and `internalWidth/internalHeight` are the scene. The read-only
`settings.display` service exposes these values from the latest recorded frame.
They may be zero before a first frame or lag one frame during resizing.

Reproduce a deterministic 4K comparison (DX12, RelWithDebInfo, fixed 60 Hz input,
1500 frames, first 120 excluded, VSync off; no screenshots during measurement):

```powershell
& C:/WorkSpace/HEngineProjects/MechanicGirlLab/Tools/Measure-RenderUpgrade.ps1 -Profiles performance -Resolution 3840x2160 -InternalHeight 0
& C:/WorkSpace/HEngineProjects/MechanicGirlLab/Tools/Measure-RenderUpgrade.ps1 -Profiles performance,balanced -Resolution 3840x2160 -InternalHeight 1080
```

The tool runs bounded `hectl game check` and writes per-pass, CPU/GPU, memory and
actual-resolution evidence under the external project's Saved/Diagnostics/RenderUpgrade.
`Tools/Agent/Analyze-RenderResolution.py` preserves the writer's warmup boundary and
deduplicates asynchronous GPU samples. It explicitly marks old CSV internal sizes
as unknown. See engine-upgrade-validation.md for measured results and limits.
