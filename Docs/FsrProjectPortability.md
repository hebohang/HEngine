# Independent project integration

The upscaler, streaming policy, combat decisions, navigation follower and action
clock belong to the engine. A project supplies content, navigation surfaces,
perception observations and gameplay/animation responses. See
[ManagedCombat.md](ManagedCombat.md) and [TextureStreaming.md](TextureStreaming.md).

`Configure-FsrPortability.py` configures a newly created Scripted3D template. It
uses built-in box geometry, an independently generated checker texture and a
small project script. It does not copy MechanicGirlLab content or engine content.
The script builds a regional navmesh, follows a path into attack range and uses
`CombatAgent` and `ActionTimeline` to drive a visible pulse and gameplay markers.
It verifies two completed attacks against two commits; this is a portability
check, not a replacement for the main project's skinned-animation validation.

```powershell
powershell -ExecutionPolicy Bypass -File Tools/Agent/hectl.ps1 project create -Project C:/WorkSpace/HEngineDev/out/agent/fsr-portability-project -Name FSRPortability -Template Scripted3D -Json
py -3 Tools/Agent/Configure-FsrPortability.py out/agent/fsr-portability-project
powershell -ExecutionPolicy Bypass -File Tools/Agent/hectl.ps1 game build -Project C:/WorkSpace/HEngineDev/out/agent/fsr-portability-project
```

Use `hectl project current -Project <path> -Json` to obtain this project's own
binary and session paths. Do not reuse another project's executable, generated
sources or Saved directory. Obtain the built executable's SHA256 and run:

```powershell
powershell -ExecutionPolicy Bypass -File Tools/Agent/Test-FsrPortability.ps1 -Project C:/WorkSpace/HEngineDev/out/agent/fsr-portability-project -ExistingBuildSha256 <sha256> -GraphicsApi DX12 -OutputDirectory out/agent/captures/fsr-portability-DX12
powershell -ExecutionPolicy Bypass -File Tools/Agent/Test-FsrPortability.ps1 -Project C:/WorkSpace/HEngineDev/out/agent/fsr-portability-project -ExistingBuildSha256 <sha256> -GraphicsApi Vulkan -OutputDirectory out/agent/captures/fsr-portability-Vulkan
```

Each run checks the live upscaler, TAA exclusion/restoration, all five quality
modes, resize to 1440p/2160p and back, and a 512 -> 1 -> 512 MiB texture-budget
cycle. The tiny pressure budget is intentional: it must reduce the independently
authored texture's residency and then recover. Budget accounting is texture
payload plus pending/retiring replacements, not physical VRAM. The test does not
exercise actual device allocation failure, particles, native UI or skinned motion;
those have separate integration tests.

The driver owns and stops each Game process in `finally`, preserves logs and
captures, and records success only after clean shutdown. Inspect compact image
previews separately before claiming visual acceptance. The engine workspace's
selected project is unchanged. Exact results and remaining limits are recorded in
[FSRStreamingCombatWork.md](FSRStreamingCombatWork.md).
