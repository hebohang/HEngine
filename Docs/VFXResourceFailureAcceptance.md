# Runtime effect dependency/version admission

LoadEffectInternal now validates the EFKE envelope before Effect::Create: version
0, bounded chunk headers/sizes and one BIN_ chunk. Raw SKFE remains supported.
Both forms require SKFE magic and binary version 0..1810, matching the pinned SDK's
nonnegative legacy range and Version18 upper bound. A read-only corpus scan found
91 EFKE-0 entries and one raw SKFE-1710 entry; one archive was unreadable, so that
archive is not compatibility evidence. This does not impose ToolVersion metadata
or require a recipe/provenance file.

After SDK creation, before cache publication, every declared nonempty color,
normal/distortion texture, model, material, sound and curve path must resolve to a
nonempty package entry and a nonnull SDK resource. Empty optional paths are skipped.
Procedural model parameters, when present, must have a generated model. Requiring
the package entry prevents ResourceManager's same-name cached objects from hiding
a missing dependency. Material source fallback remains valid: the optional
.efkmatd probe is not a failure when the .efkmat source produces a material.

The checks use SDK resource lists from BIN_, not INFO/provenance alone. Unused
INFO-only annotations are not newly enforced. Nested resource contents and general
malformed SKFE fields still rely on the SDK parser; this is not a full replacement
binary validator. No reflection fields or public APIs changed. Existing load/reload
transactions perform rollback on rejection, preserving active EffectRefs and the
previous cache entries. Updating bytes of a present same-path texture while its
SDK resource is cached is a separate resource-cache invalidation concern.

Reproduction (main agent only; script never builds):

```powershell
$python='C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe'
& $python Tools/VFX/resource_failure_acceptance.py --backend DX12 --existing-build-sha256 '<candidate SHA256>' --output out/agent/rendering-evidence/vfx-authoring/resource-failure-dx12-01
& $python Tools/VFX/resource_failure_acceptance.py --backend Vulkan --existing-build-sha256 '<candidate SHA256>' --output out/agent/rendering-evidence/vfx-authoring/resource-failure-vulkan-01
```

Each case starts a real same-key good hit-ember cache and paused old instance, then
replaces only its private scratch package with: missing PNG, missing model, EFKE
version 0x7fffffff, or SKFE version 0x7fffffff. Reload must reject while the session
remains alive. With the corrupt file still on disk, fresh Play at that same key must
return the old cached effect's radius metadata; old instance must retain playing,
paused and intensity=.5 state. PNGs are evidence, not particle-state hashes.
Finally stop handles, retire frames and stop Session before completing Storage;
only the script-owned scratch file/directory are removed. Original assets and
candidate executable hashes must remain unchanged.

Not covered by this driver: missing material, curve or sound negative fixtures,
visual approval, and particle-state determinism. Those checks are implemented but
must not be reported runtime-tested from the four cases. Preparation validation:
bundled Python --help and all four in-memory archive mutations PASS; diff check
PASS. No engine build or GPU session executed for this change.

The optional `--legacy-sampler-package '<preserved pre-migration GpuSparks20k.efkpkg>'`
adds a fifth same-key rollback case without changing the existing four negatives.
It requires the original SKFE version 1800 and enables GPU support for this run,
so GPU-disabled policy cannot masquerade as sampler validation. The final native
log must explicitly contain the built-in CPU sampler rejection with `filter=-1`
and `wrap=-1`; old instance and same-key cached fresh play checks remain required.
Use the preserved negative fixture path, never a regenerated 1810 replacement.

Runtime validates public `GetBasicRenderParameter` sampler enums before publishing
the Effect cache entry, for Sprite/Ribbon/Ring/Model/Track built-in material nodes
(including CPU+GPU hybrid nodes). Root/None and file-material samplers are outside
this check. It does not reject a binary solely because its version is 1800 and
does not clamp invalid values. Existing load/reload transactions retain the old
cache and running instances on rejection.
