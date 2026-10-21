# MechanicGirlLab 4K temporal stability and performance

Implementation and representative DX12 validation completed, 2026-09-29.
Project: `C:/WorkSpace/HEngineProjects/MechanicGirlLab`.
Hardware observed: RTX 3060 Laptop, 6144 MiB, driver 591.74. Existing working-tree
changes predate this task and are preserved. No commit or push performed.

结论：修复低分辨率 TAA 输出坐标、历史采样和细小轮廓覆盖问题，补齐可重建
的静态 LOD 资产链路。当前推荐 Balanced、内部1080p、4K无边框全屏；默认开启
垂直同步，匹配本机60Hz屏幕。最终着色器的6000帧持续输入检查中，98.83%的
测量帧满足60fps预算，CPU P95为14.79ms、GPU P95为12.18ms。固定机位四类
区域的时序波动较原始基线下降约58%–69%，连续转镜头未见明显拖尾。细小树叶
仍有亚像素时序变化，不能称为全场景数学意义上的零闪烁或严格锁60。
当前方案也不冒充 TSR、DLSS 或 Nanite。

## Findings and implemented experiments

- Current path: internal 1920x1080, TAA at internal resolution, Catmull-Rom spatial
  upscale to 3840x2160. Not TSR or temporal super resolution.
- Runtime baseline: 3103 mesh instances, 332 unique mesh paths; 0 sections with
  authored LOD; 4,179,069 candidate base/selected triangles at the captured pose.
  Engine LOD defaults off; simply enabling it cannot simplify these assets.
- TAA compared exact primitive IDs and used center-pixel brightness changes as
  reactivity, suppressing accumulation of legitimate subpixel detail. Changed
  its matching to instance/class identity with existing depth/normal/color guards,
  and reactivity to history outside the current neighborhood luminance range.
- TAA history is on the jittered grid. Added `SceneTAAReconstruct` to reconstruct
  the presentation copy on the unjittered grid before transparent overlays, leaving
  motion/history conventions intact. Jitter state participates in transactional
  rollback; subsequent Game and Editor builds and runtime checks passed.

## Evidence so far

`Tools/Agent/Measure-TemporalStability.ps1 -Label <label>` builds Game, starts an
owned fixed-step DX12 session, warms 240 simulation frames, cuts camera and settles
64 submissions, captures 16 static-camera and 16 walking images, then stops in
finally. Exact 4K output, 1080p internal. Images are not performance measurements.
Analyze with bundled Python and `Tools/Agent/Analyze-TemporalStability.py <dir>`.

External `Saved/Diagnostics/TemporalStability/` runs:

| Run | Roof mean std RGB8 | Ground | Cart | Foliage |
| --- | --- | --- | --- | --- |
| 20260929-094511-baseline | 1.596 | 2.039 | 2.212 | 2.594 |
| 20260929-094831-footprint | 1.417 | 1.707 | 1.963 | 2.582 |
| 20260929-095235-reconstructed | 0.933 | 0.813 | 1.312 | 1.967 |

These are measured variation, not truth-image error or proof of zero flicker.
Static ROI previews and later walking/turning contact sheets inspected.
Baseline history records show no unexpected resets; ROI mean brightness is stable.
First three sessions pinned with `.he-keep` to preserve comparisons:
`20260929-094521-767-ab7c478e50c3`, `20260929-094842-097-5c32f87acefa`,
`20260929-095343-151-d2bcca954f53` under project Game cache sessions.

## Research

Epic TSR reconstructs at display resolution with temporal rejection and flicker
analysis: https://dev.epicgames.com/documentation/unreal-engine/temporal-super-resolution-in-unreal-engine

NVIDIA's September 22 developer article describes DLSS 5 neural rendering running
on RTX 50 GPUs; it is not the suitable implementation target for this RTX 3060:
https://developer.nvidia.com/blog/whats-new-for-game-developers-dlss-5-with-3d-guided-neural-rendering-nvidia-ace-updates-and-new-rtx-kit-capabilities
DLSS Super Resolution is feasible through NVIDIA Streamline: the engine already
has HDR color, raster depth, motion vectors, unjittered camera matrices and explicit
cut/reset ownership. It would replace TAA and output display-resolution color;
running DLSS after TAA would filter the image twice and is not the intended path.
Native device/command-list/resource access belongs under Runtime/Platform, with
resource tagging, barriers/state restoration and capability fallback behind a
public renderer provider. A production integration still needs the official signed
SDK/runtime, dynamic-resolution/resize/scene-cut tests, character motion, transparent
reactive treatment, exposure convention and 6GB memory/performance validation.
No DLSS runtime, fake toggle or claimed DLSS support was added in this change.

Official integration requirements:
https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS.md

DLSS SR supports RTX 30, but NVIDIA warns the newer M/L models have a heavier cost
on RTX 20/30 (no native FP8). The latest model is not automatically the best choice
for this laptop's 16.67ms budget:
https://www.nvidia.com/en-us/geforce/news/dlss-4-5-super-resolution-available-now/

UE5 also offers Nanite's virtualized geometry and automatic detail selection.
This project uses conventional indexed meshes, authored index LOD and hysteresis;
it does not implement Nanite, clustered geometry streaming or skeletal LOD.
https://dev.epicgames.com/documentation/unreal-engine/nanite-in-unreal-engine

## LOD and CPU/GPU findings

`node Tools/Assets/Generate-SceneLods.mjs --project C:/WorkSpace/HEngineProjects/MechanicGirlLab --scene Assets/Imported/23952a47-64d5-5eec-b529-2f77bc4bf3f5/Scene.he --output Assets/Generated/SickaLods`
produced 97 derivative meshes with 106 levels and 10,423,896 additional index bytes.
Scene authoring binds 1790 instances after matching their original static lightmaps.
Original asset files are untouched; vertex attributes/UV1 seams, material sections
and LOD0 are preserved. Source hashes invalidate stale derivatives. Full-screen
runtime observes 596 eligible sections and 241–277 reduced sections along the replay.
One pose reduces candidate triangles from 4,179,069 to 3,855,567 (7.74%). This does
not imply equal GPU savings: pixel shading, alpha-tested foliage and skinning remain.
Display-pixel error plus the existing 80% enter threshold avoids obvious popping;
the purpose is to keep transitions visually small, not to make LOD switching obvious.

Static verification PASS:
`python Tools/Assets/Verify-SceneLods.py C:/WorkSpace/HEngineProjects/MechanicGirlLab Assets/Generated/SickaLods/manifest.json`.
Also PASS: project `Tools/Validate-SickaTreeImport.py` verifies 69 trees and four
LOD0 triangle/UV/normal/tangent sets against the original UE export.

Full-screen 3840x2160, DX12, RTX3060 Laptop, VSync off, fixed-step input replay,
1500 frames, first 120 excluded, no capture, 1380 CPU observations:
`Tools/Agent/Measure-Mechanic4K.ps1 -Cases lod-off,balanced,optimized,1440 -Frames 1500`.
Evidence: project `Saved/Diagnostics/TemporalPerformance/20260929-100950`.

| Case | CPU median/P95 ms | GPU median/P95 ms | CPU frames <=16.667ms |
| --- | --- | --- | --- |
| 1080 internal, LOD off | 13.443 / 14.869 | 11.638 / 12.121 | 98.91% |
| 1080 internal, LOD on | 13.618 / 14.732 | 11.473 / 11.933 | 99.64% |
| 1080, shared/optimized TAA | 13.644 / 15.245 | 12.083 / 12.496 | 98.12% |
| 1440 internal, LOD on | 15.967 / 17.323 | 15.861 / 16.681 | 84.06% |

Retain 1080 and the ordinary TAA gather. LOD helps geometry cost modestly; frame
times include noise and are not proof of a CPU speedup. 1440 exceeds the desired
P95 budget. The optional optimized/shared gather is slower on this GPU.
Balanced median GPU passes: GBuffer1.893ms, sun shadow1.526ms, TAA1.453ms,
post-TAA unjittered depth1.224ms, lighting0.990ms, prepass0.881ms, tone map0.560ms.
The new reconstruction costs about0.144ms. Median CPU update6.932ms,
render/submit/present6.482ms, frame preparation2.217ms, graph recording2.250ms,
animation0.752ms and deformation0.011ms. These nested timings must not be summed.
CPU thread-busy counters contain unavailable/quantized samples; no claim is based
on their median. Existing TaskScheduler already parallelizes mesh work. A wholesale
render-thread/ECS rewrite is not justified to resolve this GPU/temporal defect;
CPU scheduling and draw recording remain future profiling targets.

Final depth-guard visual run `20260929-101439-depth-guard-final`: normalized positive
history filtering replaces nearest-valid snapping on partially valid footprints,
and current neighborhood matching retains a depth boundary between mesh layers.
16 consecutive static images: roof0.758, ground0.655, cart1.015, foliage1.946 mean
std RGB8. Relative to baseline: reductions52.5%,67.9%,54.1%,25.0%. Stable camera
matrices and zero unexpected history resets are checked by the analysis script.
Walking contact sheet shows no obvious character trails in sampled frames. This
does not establish zero foliage shimmer or all-camera temporal correctness.

## Storage

Free C: space before targeted cleanup was5,516,824,576 bytes. Removed three verified
idle build caches (default Game, tests and project a01f5e672ff1b266), plus detailed
media/CSV data from completed historical upgrade-motion/aster-benchmark runs;
their JSON summaries remain. Deleted logical bytes:25,883,207,233. Free space after
those deletions was29,260,943,360 bytes (about22.1GiB physical increase); later
captures/builds consume some space. `clean retired` found no remaining retired
caches. Current project Game/Editor and pinned evidence are retained. The large
build-publish directory contains Android publication/recovery-like intermediates
whose ownership/completion was not established, so it is retained. Tool-download
folders contain no further large eligible cache. General `clean stale` was stopped
after repeated expensive inventory scans; its exit=-1 is not a successful cleanup.

## Final verification and limits

**runtime-manual PASS:**
`Tools/Agent/Measure-Mechanic4K.ps1 -Cases balanced -Frames 6000`.
Evidence `Saved/Diagnostics/TemporalPerformance/20260929-101630`.
5880 measured CPU frames: median13.0314ms, P9514.3534ms, P9915.4565ms,
max43.6587ms; 29 frames exceed16.667ms (99.5068% within budget).
5876 unique GPU samples: median11.2384ms, P9511.8405ms, max12.8072ms.
Local VRAM peak3,622,174,720 bytes; no loading frames or pending texture uploads.
The replay supplies1800 simulation frames of movement/combat, followed by released
input for the remaining run; this is not100 seconds of continuous traversal.
Largest CPU spike occurs in frame preparation with no graph rebuild. Other slow
frames occur in update. No CPU scheduling cause is asserted without a trace.

**runtime-manual PASS:**
`Tools/Agent/Measure-TemporalStability.ps1 -Label synced-final -Fullscreen -RespectPresentationSettings`.
Evidence `Saved/Diagnostics/TemporalStability/20260929-102236-synced-final`.
Effective settings confirm TAA, LOD=true, RenderHeight1080 and VSync=true. The
3840x2160 monitor is60Hz. Static, walking and turning sequences captured;
sampled character silhouettes/scene/UI show no obvious trails or displacement.
Only the static cut/settle sequence has the history-record artifact; turning is
visual evidence, not a per-frame history-reset assertion. This is not scan-out
capture and cannot measure physical display tearing; VSync is enabled to avoid it.
The performance scripts retain explicit VSync=false overrides. RespectPresentationSettings
is opt-in for owned visual sessions and does not change performance-test defaults.

**runtime-manual PASS (includes build):**
`.\hectl.bat editor capture -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab -StartupScene Assets/Scenes/SickaDynasty.he -Frames 360 -Resolution 1920x1080 -GraphicsApi DX12 -StrictGraphicsApi -RenderValidation -CapturePath C:/WorkSpace/HEngineProjects/MechanicGirlLab/Saved/Diagnostics/TemporalStability/editor-final.png -TimeoutSeconds 300`.
Editor139.0s build, exit0; scene, materials, lighting and panels inspected in a
1280px preview. NVRHI validation enabled, no WARN/ERROR. Native D3D12 debug layer
unavailable, explicitly reported; no native-debug-layer coverage is claimed.

**runtime-manual PASS:**
`& C:/WorkSpace/HEngineProjects/MechanicGirlLab/Tools/ValidateDisplayDropdowns.ps1 -ArtifactPath C:/WorkSpace/HEngineDev/out/agent/projects/7e44ffe2e91e72bd/game-relwithdebinfo/bin/Gamee4fb9eeb.exe`.
Evidence `Saved/Diagnostics/DisplayDropdowns/20260929-102410`, binary SHA256
`4679018d7772478b35d7152866859ea7c796567cedd5129a1c823f3b21d2e053`.
Virtual mouse verified quality cancel/apply, window/fullscreen preferences,
native/720/1080 graph extents, FXAA/SMAA/TAA transactions and return to main menu.
Representative TAA UI preview inspected. This script deliberately disables VSync;
the separate synced-final session above verifies the actual project default.

**static-check PASS:** LOD verification and repeat generation manifest equality;
four modified/new PowerShell scripts parse; three new Python scripts parse;
bundled Node `--check Tools/Assets/Generate-SceneLods.mjs`; `git diff --check`.
The machine's unrelated global Node syntax-check invocation rejected module mode;
the bundled Node check passed. Runtime generator executions also passed.
Image metrics use:
`& C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe Tools/Agent/Analyze-TemporalStability.py C:/WorkSpace/HEngineProjects/MechanicGirlLab/Saved/Diagnostics/TemporalStability/20260929-101439-depth-guard-final`.

All owned Game/Editor processes stopped, including synced pid121604 and UI pid67408.
No Vulkan runtime, Shipping/cook, DLSS integration or claim of universal zero flicker.
Residual thin-foliage aliasing, higher-resolution temporal reconstruction, skeletal
LOD and wider CPU scheduling remain explicit future improvements, not delivered
features. Native4K is not recommended for this laptop's60fps target.

## Next goal continuation

Do not report the overall no-flicker goal complete yet. The remaining visible
thin-foliage variation needs a targeted static/animated separation and coverage
analysis, then a justified reconstruction/temporal-upscaling improvement. Preserve
the measured1080p performance headroom. Add a genuinely continuous traversal and
turning performance replay; the6000-frame run above contains only1800 frames of
scripted input. Current source is built and checked, no owned processes remain,
and all necessary history is summarized here; do not reload old bulk media.

## Coverage and continuous-input follow-up

The earlier next-step items above are a historical checkpoint. On the next pass,
the performance driver was corrected to repeat the authored gameplay portion
after its initial 120-frame startup. It now emits a bounded replay whose frame
count equals the requested run, rather than releasing input after frame1800.
The 6000-frame replay contains172 segments. It exercises alternating movement,
sprint and combat; it does not represent unrestricted traversal of every area.

**runtime-manual PASS:**
`Tools/Agent/Measure-Mechanic4K.ps1 -Cases balanced -Frames 6000`, evidence
`Saved/Diagnostics/TemporalPerformance/20260929-103323` (before coverage change).
CPU5880 frames: median13.0731ms, P9514.4916ms, P9915.6969ms,
39 frames over16.667ms (99.3367% within). GPU5876 samples:
median11.1780ms, P9511.8098ms, max12.4447ms. No loading frames/pending uploads.

For the raster TAA path, a3x3 nearest-supported-surface guide now follows a pixel
footprint crossing instance/sky boundaries. The current center color remains the
coverage observation; its clipping neighborhood includes foreground/background
coverage. History still validates instance/class, previous depth and normal.
Unsupported center surfaces remain current-only. The temporal-GI path keeps its
original same-surface behavior. This addresses repeated rejection as thin leaf
coverage moves between the jittered foreground and background samples.

**runtime-manual PASS:**
`Tools/Agent/Measure-TemporalStability.ps1 -Label coverage-experiment -Fullscreen`.
Evidence `Saved/Diagnostics/TemporalStability/20260929-103541-coverage-experiment`.
Static16 frames have a constant camera and no unexpected history resets.
ROI mean stdRGB8: roof0.6106, ground0.6413, cart0.8790, foliage1.1013.
Foliage variation is43.4% lower than depth-guard-final and57.5% below the original
baseline. The same metrics are not ground-truth quality or zero-flicker proofs.
The foliage/moving-character comparison preview was inspected; thin coverage is
smoother, with some expected loss of contrast at subpixel silhouettes.

**runtime-manual PASS:**
`Tools/Agent/Measure-Mechanic4K.ps1 -Cases balanced -Frames 1500`, coverage shader,
evidence `Saved/Diagnostics/TemporalPerformance/20260929-103645`.
CPU median13.6819ms, P9515.3667ms,97.8261% within16.667ms;
GPU median11.8738ms, P9512.2749ms, max13.0365ms. TAA median1.845ms,
about0.39ms above the earlier balanced pass. Both shaders' source hashes are now
recorded alongside new evidence, because the executable hash alone does not
identify shader source changes.

The old turning sequence changed the pointer once and then captured16 frames;
it tested settling after a turn. The driver now changes the pointer before each
single simulation step and capture. Its first attempt failed the diagnostic
schema (`capture.sequence` requires8..16 frames); the owned session was stopped
in finally. The revised driver uses `capture.frame` after each step instead.
This diagnostic failure is not a game/runtime-rendering failure.

The single-frame revision also proved insufficient for a strict temporal test:
its recorded application-frame IDs contained gaps because asynchronous captures
allow intervening render submissions. Those images are not claimed as consecutive
rendered frames. The final implementation adds optional `steps/stepX/stepY` to
the development-only `input.pointer` command. It validates the whole bounded
trajectory before mutation, publishes one position per simulation step, and
clears remaining input on clear/stop. Existing single-position calls keep their
behavior. The measurement driver now uses this path with `capture.sequence`.

## Final follow-up evidence

**runtime-manual PASS (includes Game build):**
`Tools/Agent/Measure-TemporalStability.ps1 -Label coverage-atomic-turn -Fullscreen -RespectPresentationSettings`.
Evidence `Saved/Diagnostics/TemporalStability/20260929-104316-coverage-atomic-turn`.
The16 turning captures have consecutive simulation steps274..289. The0/5/10/15
contact sheet shows actual progressive camera rotation; character silhouettes,
columns and foliage have no obvious long trails in the inspected frames.
Static ROI mean stdRGB8: roof0.6094, ground0.6414, cart0.8778, foliage1.0999.
Static camera/history contracts valid, no unexpected resets. TAA/LOD/VSync enabled,
3840x2160 output and1920x1080 internal. This remains runtime visual/metric evidence,
not an automated perceptual-quality assertion or physical scan-out measurement.

**runtime-manual PASS (includes Game build):**
`Tools/Agent/Measure-Mechanic4K.ps1 -Cases balanced -Frames 6000`.
Evidence `Saved/Diagnostics/TemporalPerformance/20260929-104102`; final coverage
shader, no screenshot/readback, VSync off. CPU5880 measured frames:
median13.1610ms, P9514.7915ms, P9917.0562ms, max34.6943ms;
69 over16.667ms,98.8265% within budget. GPU5876 unique samples:
median11.4125ms, P9512.1789ms, max13.1523ms. No loading frames/pending uploads.
The subsequent C++ change only adds the optional development pointer trajectory;
ordinary key-only replay does not use it. Shader hashes accompany this run.

**runtime-manual PASS (includes Editor build):**
`.\hectl.bat editor capture -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab -StartupScene Assets/Scenes/SickaDynasty.he -Frames 240 -Resolution 1920x1080 -GraphicsApi DX12 -StrictGraphicsApi -RenderValidation -CapturePath C:/WorkSpace/HEngineProjects/MechanicGirlLab/Saved/Diagnostics/TemporalStability/editor-coverage-final.png -TimeoutSeconds 300`.
Build28.4s, exit0, preview inspected. No WARN/ERROR/FATAL in run logs.

The practical60fps target is met for the measured hardware/profile and replay,
with remaining CPU spikes and subpixel foliage variation disclosed. No claim is
made for all viewpoints, native4K, Vulkan runtime, Shipping, DLSS integration or
new general engine multithreading. The existing task scheduler was inspected;
the measured frame budget did not justify a broad threading rewrite for this fix.
