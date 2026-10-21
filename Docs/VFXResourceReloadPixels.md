# Same-path resource reload isolation

Runtime Initialize now calls the official
`Manager->GetSetting()->SetIsFileCacheEnabled(false)`. Setting forwards this to
ResourceManager::SetIsCacheEnabled, disabling its texture/model/material/sound/
curve/vector-field and procedural-model caches. Runtime package and whole-Effect
caches remain enabled, so repeated Play of an already loaded effect shares that
effect's resources. Different loaded effects and old/new reload versions own
independent resources; this increases resource allocations, load work and memory
where effects previously shared dependencies. No performance improvement is
claimed. Measure the changed costs separately after correctness acceptance.

The SDK's prior CachedResources key was only the virtual path. A reload could
return old texture bytes while the old instance kept that cache entry alive;
unrelated packages using the same internal filename could also alias. Bypassing
that cache avoids mutating old resources and retains transactional Effect cache
rollback. The public noncached load path does not assign a cached resource path;
its matching Unload routes directly to the loader, retaining normal ownership.

Independent probe (does not alter existing resource-failure cases):

```powershell
$python='C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe'
& $python Tools/VFX/resource_reload_pixels.py --backend DX12 --existing-build-sha256 '<candidate SHA256>' --output out/agent/rendering-evidence/vfx-authoring/resource-reload-dx12-01
& $python Tools/VFX/resource_reload_pixels.py --backend Vulkan --existing-build-sha256 '<candidate SHA256>' --output out/agent/rendering-evidence/vfx-authoring/resource-reload-vulkan-01
```

The script copies CpuSpriteControl into private scratch packages. It preserves the
original texture alpha and all effect fields, changing only RGB to controlled red,
green or blue at the same internal filename. Package A starts red at x=-1.4 and
is paused after 8 steps. A is rewritten green and reloaded at the same path;
a new instance at x=+1.4 must be green while the old one remains red. The green
instance is stopped while its Effect remains cached; package B with the same
internal filename is played at x=+1.4 and must be blue, with old A still red.

The old screen half is inferred from its first image; the other half tests new
instances. Signal differs from empty background by >20/255. Dominant color requires
channel >40 and >15 above each other channel. Each tested instance needs at least
50 signal pixels and 70% expected-color dominance. Old-half RGB mean difference
must remain <=2/255 across reload and other-package play. All images, stats,
thresholds and SDK status evidence remain on disk. Overall visual status is always
REQUIRES_REVIEW; quantitative thresholds are not particle-state hashes or a claim
that model/material/audio content reload has separately been observed.

Session pins the candidate hash, enables RenderValidation through preview.Session,
and uses outer Storage. Finally stops handles, advances retirement frames, stops
the owned process and removes only the two private scratch package files. Original
asset and executable hashes are checked. Preparation validation only: help/import
and in-memory texture/metric checks; no engine build or GPU process was run.
