# SVGF kernel A/B comparison

`Rendering.GI.SVGF.OptimizedKernels` defaults to `false`. Set it in a stable-ID render-settings JSON patch and pass the file through `-RenderSettingsPath` / `--render-settings`. The old and optimized branches coexist in the same shader artifact and use the same resolution, samples, guides, 25-tap A-trous stencil and configured iteration count.

The enabled branch performs three changes:

1. An invalid center material in `HistoryFix` or `ATrousFilter` immediately writes the existing center fallback. Every original neighbor would fail the invalid-material test. The `HistoryFix` mature-history fast path and its original alpha semantics remain intact.
2. `TemporalReprojection` skips neighborhood clamp statistics when it has no valid history or current observation. Those statistics only affect the valid observed-history clamp. The common tail still writes every history channel and preserves the existing missing-observation retention/aging behavior.
3. A-trous depth, luminance, hit-distance, albedo and roughness exponential weights are combined into one exponential of their summed exponents. The normal power, confidence, history and spatial weights are unchanged. This is mathematically equivalent; floating-point rounding and extreme underflow can differ. Image differences and motion stability must be checked before making it the default.

The flag is captured in immutable graph-pass data so candidate graph preparation does not change the old graph's kernel selection. `TemporalConstants` and `AtrousConstants` reuse their existing padding and remain 48 bytes. `HistoryFixConstants` grows from 32 to 48 bytes, with matching C++/HLSL layouts and NVRHI binding sizes.

## Audited pre-optimization evidence

Source: `out/agent/rendering-evidence/modern-ssgi-validation.csv` and its `.passes.csv` sidecar. The startup log identifies `Assets/RenderingBenchmarks/Validation/SurfaceCoverage.he`. Native 3840×2160, DX12 on the RTX 3060 Laptop GPU, validation enabled, VSync disabled, SSGI 0.5 scale (1920×1080), 2 rays, 32 steps, SVGF 4 iterations and TAA. The effective-settings sidecar records the full quality configuration and `Native4KComparable.json` input; its recorded phase is `startup`, not a final runtime snapshot.

CPU recording covers frames 61–120 after 60 warmup frames. Using the exported `gpuWarmupCutoffSample=75`, deduplicating GPU sample IDs and accepting only IDs greater than 75 gives 56 GPU samples (76–131). Percentiles use the repository's `Get-RenderSampleStatistics` linear interpolation at `(N - 1) * percentile`, rather than nearest-rank selection. Whole-graph GPU mean is 25.200 ms, P95 25.406 ms, P99 25.470 ms and maximum 25.479 ms; all 56 accepted samples exceed both 60 FPS and 90 FPS time budgets. This interval measures the RenderGraph, excluding Present and work outside the graph. The 60 CPU wall samples give mean 25.258 ms, P95 28.040 ms, P99 29.728 ms and maximum 30.138 ms; CPU and GPU durations must not be added together.

Pass means use those same accepted GPU IDs. The four A-trous passes average about 3.69 / 3.37 / 3.20 / 3.19 ms, together about 13.46 ms; ray marching averages about 0.50 ms. Temporal reprojection averages 1.19 ms, history fix 1.09 ms, bilateral upsample 0.69 ms and TAA 3.60 ms. Transient pool reservation is 3,828,429,108 bytes across three frame slots, with 1,276,143,036 bytes reported per-frame peak; these are pool counters, not measured total device VRAM usage.

This run contains a subsequently fixed NVRHI shadow push-constant validation error and is therefore **validation FAIL**, despite normal process exit. It is diagnostic performance evidence and must be rerun for acceptance. The separate `modern-integration-validation` GI-off run used FXAA, so its approximately 3.3 ms total is not a same-AA comparison against the TAA run.

This is a short diagnostic run, not evidence of thermal stability. The cited CSV does not establish the power/performance mode, sustained GPU clocks, temperature plateau, total VRAM/system-memory peak, asset hashes or absence of concurrent GPU workloads. A complete benchmark report must record those conditions and the exact invocation separately. Later reruns should use distinct evidence filenames so the failed diagnostic data remains identifiable.

No post-optimization performance or image-quality PASS is claimed here. Paired runs must use identical settings except this flag, the same camera/frame sequence, full warmup and no concurrent GPU workloads. Preserve the effective-settings sidecars, full-frame GPU-span percentiles, individual pass samples, memory peaks, validation logs and static/motion image comparisons. Keep ray count and filter iterations fixed when evaluating this optimization.

## 2026-09-08: shared guide cache (v2)

The opt-in flag now selects a separate `ATROUS_CACHED` compute shader for iterations 0 and 1 (steps 1 and 2). An 8x8 group cooperatively loads a 12x12 or 16x16 full-float guide tile before any out-of-bounds return. Material IDs remain exact uints; invalid taps are rejected. Iterations 2 and later use the reference-layout shader. The reference variant has no groupshared allocation. This is additional to the three arithmetic/early-exit changes above; the 25 taps, guides, working resolution and configured iteration count are unchanged.

Validation evidence is in `out/agent/rendering-evidence/`:

- `svgf-cache-vulkan-odd-2` and `svgf-cache-reference-vulkan-odd`: native1281x721, validation PASS. Candidate/reference maxRGBdiff1;99.99935037% channels identical. First attempt failed shader compilation because the fallback entry guard omitted the new define; corrected before these runs.
- `svgf-cache-candidate-dx12-4k` and `svgf-cache-reference-dx12-4k`: native3840x2160,180frames/60warmup, validation PASS. MaxRGBdiff1;99.99654787% channels identical. Reference GPU25.5213ms vs candidate20.3791ms; ATrous0 3.7199->1.2853ms, ATrous1 3.3857->1.4418ms. All120 measured CPU frames still exceeded16.667ms.
- `intel-svgf-cache-deferred-1800`:600warm+1200measure; CPU46.9506/P9550.4912/P9951.7104ms; GPU46.9062/P9547.6915/P9948.0334ms. Short diagnostic only, not a thermally matched A/B. All1200frames over60budget.

These numeric comparisons do not establish complete motion quality. Full switching/RTGI regression with v2 enabled and sustained complex-scene A/B remain required. The default stays false.

## 2026-09-08: step-4 cache extension and rejected step-8 experiment

Current code also caches iteration2 (step4). Each group covers output pixels with one residue modulo4 in each axis. This makes a12x12 shared tile sufficient for all25 taps of8x8 outputs; padded dispatch groups cover odd image extents, and all threads fill the tile before any bounds/background return. Full-float data, tap order, material IDs, clamp coordinates and working dimensions are preserved. Iteration3 and later retain the reference layout.

The initial step4+step8 experiment (`svgf-strided-dx12`, same-binary `svgf-strided-reference-dx12`, native4K180/60warm) passed NVRHI validation and had maximum RGB difference1/255,99.996431327% identical channels. GPU means: reference25.446ms vs experiment20.604ms. ATrous2 improved3.236->2.304ms, but ATrous3 regressed3.203->4.222ms. Step8 was therefore removed from dispatch selection. This is a concrete rejected performance experiment, not a pending default enablement. Texture locality is a plausible explanation; no cache-hit hardware counters were collected.

`svgf-step4-vulkan-odd` / `svgf-step4-reference-vulkan-odd` test the retained candidate at1281x721 with2frames in flight. Build/run/NVRHI checks PASS, maximum RGB difference1,99.999350369% identical channels. The separate coordinate check `python out/agent/rendering-evidence/svgf-strided-mapping-check.py` passes34dimension/stride cases including1px and odd edges: each valid output exactly once and each clamped axis tap exactly matches the reference. This synthetic check does not replace GPU execution. Native4K full Vulkan regression is in progress; final complex-scene/thermal measurement remains required.
